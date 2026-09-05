/* fpm-ng: pool.type = supervisor.
 *
 * Patrz fpm_pool_supervisor.h i docs/NOTES.md dla uzasadnienia projektowego.
 * W skrocie: supervisor.processes mapuje sie na pm = static + pm.max_children,
 * wiec spawnowanie i wskrzeszanie procesow to cala robota fpm_children.c
 * (nietkniete, zgodnie z kontraktem z NOTES.md 3h). To, czego fpm_children.c
 * NIE potrafi, to WSTRZYMANIE wskrzeszenia (respawnuje natychmiast i
 * bezwarunkowo) — dlatego polityka restart/backoff/restart_max/fatal zyje
 * w pamieci dzielonej per pool i jest sprawdzana PRZEZ DZIECKO przy starcie
 * i miedzy kolejnymi wykonaniami skryptu, a nie przez zmiane fpm_children.c.
 */

#include "fpm_config.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "php.h"
#include "php_main.h"
#include "php_variables.h"
#include "SAPI.h"
#include "zend_globals.h"
#include "fopen_wrappers.h"

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_supervisor.h"
#include "fpm_cleanup.h"
#include "fpm_shm.h"
#include "fpm_stdio.h"
#include "zlog.h"

/* Lista ODRZUCEN, nie dopuszczen — patrz fpm_pool_type_check_directives().
 * Nazwa konczaca sie kropka lapie cala rodzine (np. "pm." lapie
 * pm.max_children, pm.start_servers, ...).
 *
 * "pm" i "pm." odrzucone w calosci: supervisor.processes JEST pm.max_children
 * pod maska (patrz fpm_pool_supervisor_validate), wiec pozwolenie userowi
 * ustawic pm.* rownolegle dawaloby dwa zrodla prawdy dla tej samej liczby.
 * "listen" i "listen." odrzucone w calosci: ten typ nie nasluchuje niczego.
 * "ping." i "access." tez odrzucone w calosci — bez listen nie ma czego
 * pingowac ani logowac jako "dostep". */
const char *const fpm_pool_supervisor_rejects[] = {
	"listen",
	"listen.",
	"pm",
	"pm.",
	"request_terminate_timeout",
	"request_terminate_timeout_track_finished",
	"request_slowlog_timeout",
	"request_slowlog_trace_depth",
	"slowlog",
	"ping.",
	"access.",
	"security.limit_extensions",
	NULL
};

/* Stan wspoldzielony miedzy WSZYSTKIMI procesami tego poola (a wiec przezywa
 * i respawny po crashu, i kolejne "iteracje" w tym samym procesie). */
struct fpm_supervisor_shared_s {
	unsigned failures;			/* kolejne "szybkie" smierci/porazki z rzedu */
	time_t next_allowed_start;		/* epoch; 0 albo przeszlosc = mozna startowac zaraz */
	unsigned char terminal;			/* 1 = polityka mowi "koniec prob na dobre" */
	unsigned char gave_up;			/* 1 = terminal z powodu wyczerpania restart_max
						 * (prawdziwa porazka - patrz supervisor.fatal),
						 * 0 = terminal bo restart=never/on-failure+sukces
						 * (planowe zakonczenie, NIE porazka) */
	unsigned char fatal_signaled;		/* SIGTERM do mastera wyslany juz raz (idempotencja) */
};

struct fpm_supervisor_registry_s {
	struct fpm_worker_pool_s *wp;
	struct fpm_supervisor_shared_s *shared;
	struct fpm_supervisor_registry_s *next;
};

static struct fpm_supervisor_registry_s *supervisor_registry = NULL;
static int supervisor_cleanup_registered = 0;

static volatile sig_atomic_t supervisor_term_requested = 0;
static volatile sig_atomic_t supervisor_stop_timeout = 10;

static void fpm_pool_supervisor_sigterm(int signo)
{
	(void) signo;
	if (!supervisor_term_requested) {
		supervisor_term_requested = 1;

		/* Siatka bezpieczenstwa: jesli biezaca iteracja (skrypt, ktory nie
		 * sprawdza niczego miedzy wlasnymi krokami) nie skonczy sie sama w
		 * ciagu stop_timeout, ubijamy sie sami sygnalem KILL.
		 *
		 * CELOWO nie uzywamy tu alarm()/SIGALRM: PHP samo uzywa SIGALRM (albo
		 * SIGPROF, w zaleznosci od budowania) do wlasnego max_execution_time
		 * (zend_set_timeout_ex(), Zend/zend_execute_API.c) i pod
		 * ZEND_SIGNALS re-instaluje ten handler przy KAZDYM wykonaniu
		 * skryptu — nasz raw sigaction(SIGALRM,...) zostalby po cichu
		 * podmieniony i strzelilby w rece Zenda zamiast w nasze (zmierzone:
		 * budowa tego projektu ma -DZEND_SIGNALS). Zamiast tego forkujemy
		 * malutki proces-watchdog, calkowicie niezalezny od stanu sygnalow
		 * PHP: spi stop_timeout sekund, i jesli my (proces supervisora)
		 * nadal zyjemy, ubija nas SIGKILL-em. fork() jest async-signal-safe,
		 * wiec robimy to bezpiecznie tu, w handlerze, zeby zlapac takze
		 * skrypt, ktory nigdy nie odda sterowania z powrotem do naszej petli. */
		pid_t me = getpid();
		pid_t watchdog = fork();

		if (watchdog == 0) {
			sleep((unsigned) supervisor_stop_timeout);
			if (kill(me, 0) == 0) {
				kill(me, SIGKILL);
			}
			_exit(0);
		}
	}
}

/* }}} sygnaly */

int fpm_pool_supervisor_validate(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_worker_pool_config_s *c = wp->config;

	if (!c->supervisor_script || !*c->supervisor_script) {
		zlog(ZLOG_ALERT, "[pool %s] pool.type = supervisor requires supervisor.script", c->name);
		return -1;
	}

	if (!c->supervisor_restart || !*c->supervisor_restart) {
		free(c->supervisor_restart);
		c->supervisor_restart = strdup("always");
		if (!c->supervisor_restart) {
			return -1;
		}
	} else if (strcmp(c->supervisor_restart, "always") &&
			strcmp(c->supervisor_restart, "on-failure") &&
			strcmp(c->supervisor_restart, "never")) {
		zlog(ZLOG_ALERT, "[pool %s] supervisor.restart must be 'always', 'on-failure' or 'never', got '%s'",
			c->name, c->supervisor_restart);
		return -1;
	}

	if (c->supervisor_processes < 1) {
		c->supervisor_processes = 1;
	}
	if (c->supervisor_restart_delay < 1) {
		c->supervisor_restart_delay = 1;
	}
	if (c->supervisor_restart_delay_max < c->supervisor_restart_delay) {
		c->supervisor_restart_delay_max = c->supervisor_restart_delay;
	}
	if (c->supervisor_stop_timeout < 1) {
		c->supervisor_stop_timeout = 10;
	}
	if (c->supervisor_restart_max < 0) {
		c->supervisor_restart_max = 0;
	}

	/* Decyzja projektowa (patrz NOTES.md): supervisor.processes mapuje sie na
	 * pm = static + pm.max_children, zeby spawnowanie/wskrzeszanie N procesow
	 * bylo za darmo z istniejacej maszynerii fpm_children.c. Uzytkownik nie
	 * ustawia pm.* sam (odrzucone przez rejects powyzej), wiec nie ma tu
	 * konfliktu dwoch zrodel prawdy. */
	c->pm = PM_STYLE_STATIC;
	c->pm_max_children = c->supervisor_processes;

	return 0;
}
/* }}} */

static void fpm_pool_supervisor_exit_main(int which, void *arg) /* {{{ */
{
	struct fpm_supervisor_registry_s *e;

	(void) which;
	(void) arg;

	/* Wolane z FPM_CLEANUP_PARENT_EXIT_MAIN, tuz przed exit(FPM_EXIT_OK)
	 * w fpm_pctl_exit() (fpm_process_ctl.c, nietkniety). Jesli ktorykolwiek
	 * pool supervisora poddal sie z powodu prawdziwej porazki i ma
	 * supervisor.fatal=yes, ubijamy caly proces tutaj z niezerowym kodem —
	 * inaczej fpm_pctl_exit() i tak zakonczylby kodem 0. */
	for (e = supervisor_registry; e; e = e->next) {
		if (e->shared->gave_up && e->wp->config->supervisor_fatal) {
			zlog(ZLOG_ALERT, "[pool %s] supervisor.fatal: master is going down with a non-zero exit code",
				e->wp->config->name);
			_exit(FPM_EXIT_SOFTWARE);
		}
	}
}
/* }}} */

int fpm_pool_supervisor_init_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_supervisor_registry_s *entry;
	struct fpm_supervisor_shared_s *shared = fpm_shm_alloc(sizeof(*shared));

	if (!shared) {
		zlog(ZLOG_ERROR, "[pool %s] supervisor: cannot allocate shared memory", wp->config->name);
		return -1;
	}

	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		return -1;
	}
	entry->wp = wp;
	entry->shared = shared;
	entry->next = supervisor_registry;
	supervisor_registry = entry;

	if (!supervisor_cleanup_registered) {
		if (0 > fpm_cleanup_add(FPM_CLEANUP_PARENT_EXIT_MAIN, fpm_pool_supervisor_exit_main, 0)) {
			return -1;
		}
		supervisor_cleanup_registered = 1;
	}

	return 0;
}
/* }}} */

static struct fpm_supervisor_shared_s *fpm_pool_supervisor_shared_for(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_supervisor_registry_s *e;

	for (e = supervisor_registry; e; e = e->next) {
		if (e->wp == wp) {
			return e->shared;
		}
	}
	return NULL;
}
/* }}} */

static void fpm_pool_supervisor_wait(time_t seconds) /* {{{ */
{
	while (seconds-- > 0 && !supervisor_term_requested) {
		sleep(1);
	}
}
/* }}} */

static void fpm_pool_supervisor_park(void) /* {{{ */
{
	/* Ten proces jest respawnem po tym, jak inny proces tego poola juz raz
	 * zdecydowal "koniec". Nie robimy nic i nie znikamy — gdybysmy zaraz
	 * exit()owali, fpm_children.c odrodzilby nas natychmiast w petli bez
	 * konca. Sen do pierwszego sygnalu jest tani i nieszkodliwy. */
	while (!supervisor_term_requested) {
		pause();
	}
}
/* }}} */

/* Nadpisania sapi_module na czas zycia tego procesu. Bezpieczne WYLACZNIE
 * dlatego, ze ten proces nigdy nie wraca do petli accept FastCGI (child_main
 * "nie wraca") — nie ma innego kodu w tym procesie, ktory polegalby na
 * oryginalnych wskaznikach cgi_sapi_module. SG(server_context) zostaje NULL
 * przez caly czas, wiec oryginalne wersje tych callbackow (ktore go
 * bezwarunkowo rzutuja na fcgi_request*) by segfaultowaly. */
static size_t fpm_pool_supervisor_ub_write(const char *str, size_t str_length) /* {{{ */
{
	size_t left = str_length;

	while (left > 0) {
		ssize_t n = write(STDOUT_FILENO, str + (str_length - left), left);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (n == 0) {
			break;
		}
		left -= (size_t) n;
	}
	return str_length - left;
}
/* }}} */

static char *fpm_pool_supervisor_getenv(const char *name, size_t name_len) /* {{{ */
{
	(void) name_len;
	return getenv(name);
}
/* }}} */

static size_t fpm_pool_supervisor_read_post(char *buffer, size_t count_bytes) /* {{{ */
{
	(void) buffer;
	(void) count_bytes;
	return 0;
}
/* }}} */

static char *fpm_pool_supervisor_read_cookies(void) /* {{{ */
{
	return NULL;
}
/* }}} */

static void fpm_pool_supervisor_register_server_variables(zval *track_vars_array) /* {{{ */
{
	/* Brak requestu HTTP, wiec brak PHP_SELF i innych CGI-owych zmiennych —
	 * tylko srodowisko, jak w CLI. */
	php_import_environment_variables(track_vars_array);
}
/* }}} */

static void fpm_pool_supervisor_install_sapi_overrides(void) /* {{{ */
{
	sapi_module.pre_request_init = NULL;
	sapi_module.ub_write = fpm_pool_supervisor_ub_write;
	sapi_module.getenv = fpm_pool_supervisor_getenv;
	sapi_module.read_post = fpm_pool_supervisor_read_post;
	sapi_module.read_cookies = fpm_pool_supervisor_read_cookies;
	sapi_module.register_server_variables = fpm_pool_supervisor_register_server_variables;
}
/* }}} */

/* Jedno wykonanie skryptu: dokladnie wzorzec fpm_main.c (php_request_startup /
 * php_fopen_primary_script / php_execute_script / php_request_shutdown), tylko
 * bez SG(request_info) wypelnianego z FastCGI. Zwraca EG(exit_status) skryptu
 * (0 = sukces, jak exit()/normalne zakonczenie skryptu PHP; != 0 tak jak
 * exit($n) albo blad fatalny) — TO jest "kod wyjscia" dla supervisor.restart,
 * nie kod wyjscia procesu (proces nie konczy sie po kazdej iteracji). */
static int fpm_pool_supervisor_run_script(struct fpm_worker_pool_config_s *c) /* {{{ */
{
	zend_file_handle file_handle;
	int exit_code;

	SG(server_context) = NULL;
	SG(request_info).path_translated = estrdup(c->supervisor_script);
	SG(request_info).request_method = NULL;
	SG(request_info).query_string = NULL;
	SG(request_info).request_uri = NULL;
	SG(request_info).content_type = NULL;
	SG(request_info).content_length = 0;
	SG(request_info).auth_user = NULL;
	SG(request_info).auth_password = NULL;
	SG(request_info).auth_digest = NULL;
	SG(request_info).proto_num = 1000;
	SG(sapi_headers).http_response_code = 200;

	if (php_request_startup() == FAILURE) {
		zlog(ZLOG_ERROR, "[pool %s] supervisor: php_request_startup() failed", c->name);
		efree(SG(request_info).path_translated);
		SG(request_info).path_translated = NULL;
		return 255;
	}
	/* Jak przy '-v'/phpinfo w fpm_main.c: ustawic PO startupie, bo RINIT
	 * resetuje no_headers. Nie ma dokad wysylac naglowkow, wiec sciezka
	 * send_headers zostaje calkowicie pominieta (patrz sapi_send_headers()). */
	SG(headers_sent) = true;
	SG(request_info).no_headers = 1;

	EG(exit_status) = 0;

	zend_first_try {
		if (php_fopen_primary_script(&file_handle) == FAILURE) {
			zlog(ZLOG_ERROR, "[pool %s] supervisor: cannot open script '%s'",
				c->name, c->supervisor_script);
			EG(exit_status) = 255;
		} else {
			php_execute_script(&file_handle);
			if (!file_handle.in_list) {
				zend_destroy_file_handle(&file_handle);
			}
		}
	} zend_catch {
		EG(exit_status) = 255;
	} zend_end_try();

	exit_code = EG(exit_status);

	efree(SG(request_info).path_translated);
	SG(request_info).path_translated = NULL;

	php_request_shutdown((void *) 0);
	fpm_stdio_flush_child();

	return exit_code;
}
/* }}} */

/* Backoff i decyzja "czy probowac dalej", stosowana miedzy kolejnymi
 * wykonaniami skryptu w TYM SAMYM procesie, i tez przez kazdy swiezy respawn
 * po crashu (bo shared przezywa smierc procesu). */
static void fpm_pool_supervisor_apply_policy(struct fpm_worker_pool_s *wp,
		struct fpm_supervisor_shared_s *shared, int exit_code, time_t duration) /* {{{ */
{
	struct fpm_worker_pool_config_s *c = wp->config;
	int wants_retry;

	if (!strcmp(c->supervisor_restart, "never")) {
		wants_retry = 0;
	} else if (!strcmp(c->supervisor_restart, "on-failure")) {
		wants_retry = (exit_code != 0);
	} else {
		wants_retry = 1;
	}

	if (!wants_retry) {
		shared->terminal = 1;
		shared->gave_up = 0;
		shared->failures = 0;
		zlog(ZLOG_NOTICE, "[pool %s] supervisor: script finished (exit code %d), restart = %s -> not restarting",
			c->name, exit_code, c->supervisor_restart);
		return;
	}

	if (exit_code == 0) {
		/* Sukces: pod restart=always to normalny, oczekiwany koniec jednej
		 * "jednostki pracy" (skrypt sam decyduje o tempie, np. wlasnym sleep()),
		 * NIE porazka — zerujemy licznik i startujemy nastepna iteracje od
		 * razu, bez sztucznego throttlingu z naszej strony. */
		shared->failures = 0;
		shared->next_allowed_start = 0;
		return;
	}

	/* Od tego miejsca: prawdziwa porazka (exit != 0). "Zyl dostatecznie dlugo"
	 * przed porazka zeruje licznik — inaczej jeden pechowy restart po
	 * tygodniach pracy liczylby sie do tego samego restart_max co prawdziwy
	 * szybki crash-loop. Prog: restart_delay_max, czyli ta sama liczba, do
	 * ktorej i tak eskaluje backoff. */
	if (duration >= (time_t) c->supervisor_restart_delay_max) {
		shared->failures = 0;
	}
	shared->failures++;

	if (c->supervisor_restart_max > 0 && shared->failures >= (unsigned) c->supervisor_restart_max) {
		shared->terminal = 1;
		shared->gave_up = 1;
		zlog(ZLOG_ALERT, "[pool %s] supervisor: %u consecutive failures, giving up (supervisor.restart_max = %d)",
			c->name, shared->failures, c->supervisor_restart_max);
	} else {
		time_t delay = c->supervisor_restart_delay;
		unsigned i;

		for (i = 1; i < shared->failures; i++) {
			if (delay >= c->supervisor_restart_delay_max) {
				delay = c->supervisor_restart_delay_max;
				break;
			}
			delay *= 2;
		}
		if (delay > c->supervisor_restart_delay_max) {
			delay = c->supervisor_restart_delay_max;
		}
		shared->next_allowed_start = time(NULL) + delay;
		zlog(ZLOG_NOTICE, "[pool %s] supervisor: script exited (code %d) after %lds, restarting in %lds (failure %u%s)",
			c->name, exit_code, (long) duration, (long) delay, shared->failures,
			c->supervisor_restart_max > 0 ? "" : "/unlimited");
	}
}
/* }}} */

void fpm_pool_supervisor_child_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_worker_pool_config_s *c = wp->config;
	struct fpm_supervisor_shared_s *shared = fpm_pool_supervisor_shared_for(wp);
	struct sigaction sa;

	if (!shared) {
		/* Nie powinno sie zdarzyc — init_main alokuje to dla kazdego poola
		 * supervisora zanim cokolwiek sforkuje. Bez tego stanu nie ma jak
		 * bezpiecznie liczyc backoff/restart_max, wiec lepiej odmowic. */
		zlog(ZLOG_ERROR, "[pool %s] supervisor: no shared state, refusing to run", c->name);
		exit(FPM_EXIT_SOFTWARE);
	}

	supervisor_stop_timeout = c->supervisor_stop_timeout;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fpm_pool_supervisor_sigterm;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);

	fpm_pool_supervisor_install_sapi_overrides();

	if (shared->terminal) {
		/* Respawn fpm_children.c po tym, jak poprzedni proces tego poola juz
		 * raz zdecydowal "koniec". Patrz fpm_pool_supervisor_park(). */
		if (shared->gave_up && c->supervisor_fatal && !shared->fatal_signaled) {
			shared->fatal_signaled = 1;
			zlog(ZLOG_ALERT, "[pool %s] supervisor.fatal: asking the master to shut down", c->name);
			kill(fpm_globals.parent_pid, SIGTERM);
		}
		fpm_pool_supervisor_park();
		exit(shared->gave_up ? FPM_EXIT_SOFTWARE : FPM_EXIT_OK);
	}

	for (;;) {
		time_t now, started, duration;
		int exit_code;

		if (supervisor_term_requested) {
			break;
		}

		now = time(NULL);
		if (shared->next_allowed_start > now) {
			fpm_pool_supervisor_wait(shared->next_allowed_start - now);
			if (supervisor_term_requested) {
				break;
			}
		}

		started = time(NULL);
		exit_code = fpm_pool_supervisor_run_script(c);
		duration = time(NULL) - started;

		fpm_pool_supervisor_apply_policy(wp, shared, exit_code, duration);

		if (shared->terminal) {
			break;
		}
	}

	if (shared->terminal && shared->gave_up && c->supervisor_fatal && !shared->fatal_signaled) {
		shared->fatal_signaled = 1;
		zlog(ZLOG_ALERT, "[pool %s] supervisor.fatal: asking the master to shut down", c->name);
		kill(fpm_globals.parent_pid, SIGTERM);
	}

	exit(shared->terminal && shared->gave_up ? FPM_EXIT_SOFTWARE : FPM_EXIT_OK);
}
/* }}} */
