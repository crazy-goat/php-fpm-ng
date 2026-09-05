/* fpm-ng: pool.type = cron.
 *
 * Patrz fpm_pool_cron.h i docs/NOTES.md dla uzasadnienia projektowego.
 *
 * KLUCZOWA DECYZJA UPRASZCZAJACA: zadnych timerow po stronie mastera. Dziecko
 * tego poola, po starcie:
 *   1. liczy najblizszy termin z harmonogramu (fpm_cron_schedule_next()),
 *   2. spi do niego, przerywalnie — SIGTERM budzi je i konczy czysto,
 *   3. wykonuje skrypt RAZ,
 *   4. konczy proces.
 * FPM wskrzesza je istniejaca maszyneria (pm = static, pm.max_children = 1,
 * fpm_children.c respawnuje bezwarunkowo i natychmiast, NIETKNIETE), nowy
 * proces liczy kolejny termin od "teraz" i znowu spi. Dzieki temu NIE
 * dotykamy fpm_children.c ani fpm_events.c, dokladnie jak supervisor.
 *
 * Konsekwencja tej decyzji, nieoczywista, wiec zapisana wprost: cron nie ma
 * ZADNEGO stanu sterujacego w pamieci dzielonej. Minimalny stan historyczny
 * (ostatni start/wynik) istnieje wylacznie dla pool.type = status i nigdy nie
 * wplywa na zachowanie crona. Kazdy nowy proces liczy termin WYLACZNIE z
 * biezacego zegara i harmonogramu, nigdy z tego, co robil poprzednik.
 * To jest tez powod, dla
 * ktorego "nakladanie sie przebiegow" nie jest polityka, ktora trzeba
 * napisac: przy pm.max_children = 1 drugi proces tego poola fizycznie nie
 * istnieje, dopoki pierwszy nie skonczy dzialania (exit()) — fpm_children.c
 * odpala nastepny dopiero PO smierci poprzedniego. Nie ma wiec przebiegu,
 * z ktorym mialby sie nalozyc kolejny.
 *
 * CZAS: liczymy wylacznie w UTC (fpm_cron_schedule_next() uzywa gmtime_r()).
 * Czas lokalny + zmiana czasu (DST) daje przebieg podwojny albo zaden przy
 * kazdym przejsciu — nie robimy tego.
 *
 * NIE NADRABIAMY zgubionych przebiegow. fpm_cron_schedule_next() zawsze
 * liczy "co jest najblizej w przyszlosci od teraz", nigdy "co przegapilem
 * odkad ostatnio dzialalem" — jesli master byl wylaczony godzine, nastepny
 * przebieg to najblizszy przyszly termin, nie dwanascie zaleglych. Ktos
 * kiedys bedzie chcial to dodac — to bedzie zmiana projektowa, nie poprawka.
 *
 * cron.timeout uzywa DOKLADNIE tego samego mechanizmu co
 * supervisor.stop_timeout (fpm_pool_watchdog_arm(), wydzielone do
 * fpm_pool_watchdog.c) — pidfd, watchdog-fork, SIGKILL po przekroczeniu.
 * Wykonanie skryptu uzywa tej samej maszynerii co supervisor
 * (fpm_pool_script.c) — brak SG(request_info) z FastCGI, nadpisania
 * sapi_module bezpieczne z tego samego powodu (ten proces nigdy nie wraca do
 * petli accept).
 */

#include "fpm_config.h"

#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "php.h"

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_cron.h"
#include "fpm_pool_type.h"
#include "fpm_cron_schedule.h"
#include "fpm_pool_watchdog.h"
#include "fpm_pool_script.h"
#include "fpm_shm.h"
#include "zlog.h"

/* Lista ODRZUCEN, nie dopuszczen — patrz fpm_pool_type_check_directives()
 * w fpm_pool_type.c. Ten sam zestaw powodow co dla supervisora (patrz
 * fpm_pool_supervisor.c): "pm"/"pm." odrzucone w calosci, bo pm.max_children
 * jest generowane programowo (zawsze 1) i pozwolenie userowi ustawic je
 * rownolegle dawaloby dwa zrodla prawdy; "listen"/"listen." bo ten typ nie
 * nasluchuje niczego; "ping."/"access." bo bez listen nie ma czego pingowac
 * ani logowac jako "dostep"; dyrektywy requestowe (request_terminate_timeout,
 * request_slowlog_*, slowlog, security.limit_extensions) bo nie ma tu
 * requestow FastCGI; "supervisor." bo to dyrektywy DRUGIEGO typu poola. */
const char *const fpm_pool_cron_rejects[] = {
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
	"supervisor.",
	"http.",
	NULL
};

/* Stan WYLACZNIE do odczytu przez pool.type = status (docs/NOTES.md 3u).
 * W odroznieniu od supervisora, cron nadal nie ma zadnej polityki, ktora
 * czytalaby to z powrotem — kazdy nowy proces liczy termin wylacznie z
 * biezacego zegara i harmonogramu (patrz komentarz na gorze pliku),
 * niezaleznie od tego, co tu jest zapisane. Dokladnie trzy pola, tyle ile
 * status faktycznie pokazuje: next_run NIE jest tu trzymane, bo daje sie
 * policzyc w kazdej chwili z c->cron_parsed_schedule + time(NULL), bez
 * zadnego stanu — patrz fpm_pool_cron_status(). */
struct fpm_cron_shared_s {
	unsigned char running;		/* 1 = skrypt aktualnie sie wykonuje */
	time_t last_run;		/* epoch startu ostatniego przebiegu, 0 = jeszcze zaden */
	int last_exit_code;		/* kod wyjscia ostatniego ZAKONCZONEGO przebiegu */
	unsigned char has_last_exit_code;
	unsigned consecutive_failures;	/* kolejne exit_code != 0 z rzedu; na nic nie wplywa,
					 * to tylko sygnal dla czlowieka/monitoringu */
};

struct fpm_cron_registry_s {
	struct fpm_worker_pool_s *wp;
	struct fpm_cron_shared_s *shared;
	struct fpm_cron_registry_s *next;
};

static struct fpm_cron_registry_s *cron_registry = NULL;

static struct fpm_cron_shared_s *fpm_pool_cron_shared_for(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_cron_registry_s *e;

	for (e = cron_registry; e; e = e->next) {
		if (e->wp == wp) {
			return e->shared;
		}
	}
	return NULL;
}
/* }}} */

static volatile sig_atomic_t cron_term_requested = 0;

static void fpm_pool_cron_sigterm(int signo) /* {{{ */
{
	(void) signo;
	/* Tylko flaga — budzi sleep() ponizej. W odroznieniu od supervisora nie
	 * uzbrajamy tu watchdoga: nie ma "biezacej iteracji, ktora moze sie nie
	 * skonczyc sama" do pilnowania podczas SNU (nic sie nie wykonuje), a
	 * podczas WYKONYWANIA skryptu granice ustawia cron.timeout (uzbrojony
	 * osobno, patrz fpm_pool_cron_run()), nie SIGTERM. Standardowa eskalacja
	 * mastera (process_control_timeout, patrz docs/NOTES.md 3p) dotyczy tego
	 * typu tak samo jak kazdego innego. */
	cron_term_requested = 1;
}
/* }}} */

int fpm_pool_cron_validate(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_worker_pool_config_s *c = wp->config;
	struct fpm_cron_schedule_s *parsed;
	char err[256];

	if (!c->cron_schedule || !*c->cron_schedule) {
		zlog(ZLOG_ALERT, "[pool %s] pool.type = cron requires cron.schedule", c->name);
		return -1;
	}
	if (!c->cron_script || !*c->cron_script) {
		zlog(ZLOG_ALERT, "[pool %s] pool.type = cron requires cron.script", c->name);
		return -1;
	}
	if (c->cron_timeout < 0) {
		c->cron_timeout = 0;
	}

	/* Zly harmonogram = config odrzucony TERAZ, ze startu, z czytelnym
	 * komunikatem — nigdy po cichu, nigdy "mniej wiecej" w runtime. */
	parsed = calloc(1, sizeof(*parsed));
	if (!parsed) {
		zlog(ZLOG_ERROR, "[pool %s] cron: out of memory parsing schedule", c->name);
		return -1;
	}
	if (0 > fpm_cron_schedule_parse(c->cron_schedule, parsed, err, sizeof(err))) {
		zlog(ZLOG_ALERT, "[pool %s] cron.schedule '%s' is invalid: %s", c->name, c->cron_schedule, err);
		free(parsed);
		return -1;
	}
	free(c->cron_parsed_schedule);
	c->cron_parsed_schedule = parsed;

	/* Decyzja projektowa (patrz NOTES.md i fpm_pool_cron.h): cron mapuje sie
	 * na pm = static + pm.max_children = 1, ZAWSZE 1 — nie ma dyrektywy
	 * "liczba instancji" jak supervisor.processes, bo nakladanie sie
	 * przebiegow ma byc niemozliwe Z KONSTRUKCJI, a nie z polityki. Uzytkownik
	 * nie ustawia pm.* sam (odrzucone przez rejects powyzej). */
	c->pm = PM_STYLE_STATIC;
	c->pm_max_children = 1;

	return 0;
}
/* }}} */

int fpm_pool_cron_init_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_cron_registry_s *entry;
	struct fpm_cron_shared_s *shared = fpm_shm_alloc(sizeof(*shared));

	if (!shared) {
		zlog(ZLOG_ERROR, "[pool %s] cron: cannot allocate shared memory", wp->config->name);
		return -1;
	}

	/* Patrz identyczny komentarz w fpm_pool_supervisor_init_main() —
	 * cron.timeout ma dokladnie ten sam problem z SIGTERM do MASTERA:
	 * process_control_timeout globalnego mastera eskaluje do SIGKILL zanim
	 * cron.timeout zdazy cokolwiek zrobic. */
	if (fpm_global_config.process_control_timeout < wp->config->cron_timeout) {
		zlog(ZLOG_WARNING,
			"[pool %s] cron.timeout = %ds, ale global process_control_timeout = %ds; "
			"SIGTERM/SIGQUIT wyslane do MASTERA (np. `docker stop`) ubije biezacy przebieg przez "
			"eskalacje mastera, zanim cron.timeout zdazy zadzialac — ustaw process_control_timeout "
			">= %ds w [global], jesli SIGTERM/docker stop ma dac temu poolowi czas na dokonczenie przebiegu",
			wp->config->name, wp->config->cron_timeout, fpm_global_config.process_control_timeout,
			wp->config->cron_timeout);
	}

	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		return -1;
	}
	entry->wp = wp;
	entry->shared = shared;
	entry->next = cron_registry;
	cron_registry = entry;

	return 0;
}
/* }}} */

/* Spi az do `next` (epoch UTC), przerywalnie SIGTERM-em. Zwraca 1 jesli
 * doczekalismy terminu, 0 jesli obudzil nas SIGTERM (wtedy trzeba konczyc
 * czysto, bez uruchamiania skryptu). Jedno wywolanie sleep() na "skok" zamiast
 * petli co sekunde — harmonogramy miesieczne dawalyby miliony bezuzytecznych
 * przebudzen, a sleep() jest i tak przerywane KAZDYM dostarczonym sygnalem,
 * dla ktorego mamy zainstalowany handler (patrz fpm_pool_cron_sigterm()). */
static int fpm_pool_cron_sleep_until(time_t next) /* {{{ */
{
	for (;;) {
		time_t now, remaining;

		if (cron_term_requested) {
			return 0;
		}

		now = time(NULL);
		if (now >= next) {
			return 1;
		}

		remaining = next - now;
		sleep((unsigned) remaining);
	}
}
/* }}} */

void fpm_pool_cron_child_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_worker_pool_config_s *c = wp->config;
	struct fpm_cron_shared_s *shared = fpm_pool_cron_shared_for(wp);
	struct sigaction sa;
	time_t next, started;
	int exit_code;

	if (!c->cron_parsed_schedule) {
		/* Nie powinno sie zdarzyc — validate() parsuje harmonogram RAZ, przy
		 * starcie, zanim cokolwiek sforkuje. Bez tego nie ma jak policzyc
		 * kolejnego terminu, wiec lepiej odmowic niz zgadywac. */
		zlog(ZLOG_ERROR, "[pool %s] cron: no parsed schedule, refusing to run", c->name);
		exit(FPM_EXIT_SOFTWARE);
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fpm_pool_cron_sigterm;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);

	fpm_pool_script_install_sapi_overrides();

	next = fpm_cron_schedule_next(c->cron_parsed_schedule, time(NULL));
	if (next == (time_t) -1) {
		/* Ostatnia siatka bezpieczenstwa — validate() akceptuje skladnie, nie
		 * "czy harmonogram moze kiedykolwiek zajsc". Patrz fpm_cron_schedule.h. */
		zlog(ZLOG_ERROR, "[pool %s] cron: schedule '%s' never matches, refusing to run",
			c->name, c->cron_schedule);
		exit(FPM_EXIT_SOFTWARE);
	}

	if (!fpm_pool_cron_sleep_until(next)) {
		/* SIGTERM w trakcie snu — konczymy czysto, bez uruchamiania skryptu.
		 * Zaden proces-dziecko nie zostal jeszcze utworzony, wiec nie ma
		 * czego sierocic. */
		exit(FPM_EXIT_OK);
	}

	/* cron.timeout: watchdog identyczny z supervisor.stop_timeout (ten sam
	 * kod, fpm_pool_watchdog.c), tylko uzbrojony PRZED uruchomieniem skryptu
	 * zamiast w handlerze SIGTERM. Jesli skrypt skonczy sie sam w czasie —
	 * watchdog wykrywa to przez pidfd (POLLIN przy zakonczeniu PROCESU, nie
	 * skryptu — patrz fpm_pool_watchdog.h) i nic nie robi. Jesli nie — SIGKILL. */
	if (c->cron_timeout > 0) {
		fpm_pool_watchdog_arm(getpid(), (unsigned) c->cron_timeout);
	}

	started = time(NULL);
	if (shared) {
		shared->last_run = started;
		shared->running = 1;
	}

	exit_code = fpm_pool_script_run(c->name, c->cron_script);

	if (shared) {
		shared->running = 0;
		shared->last_exit_code = exit_code;
		shared->has_last_exit_code = 1;
		shared->consecutive_failures = (exit_code != 0) ? shared->consecutive_failures + 1 : 0;
	}

	/* Kod wyjscia != 0 musi byc widoczny na poziomie ostrzezenia, nie debug —
	 * pojedyncza instancja na VPS-ie nie ma klastra, ktory to wylapie. */
	if (exit_code != 0) {
		zlog(ZLOG_WARNING, "[pool %s] cron: script '%s' exited with non-zero code %d",
			c->name, c->cron_script, exit_code);
	}

	/* Zawsze normalny exit(0), niezaleznie od exit_code skryptu — cron nie ma
	 * polityki restart/backoff jak supervisor (patrz komentarz na gorze
	 * pliku), wiec exit_code skryptu nie steruje niczym poza tym logiem.
	 * fpm_children.c (pm = static, max_children = 1, NIETKNIETE) odradza
	 * ten proces bezwarunkowo i natychmiast; nowy proces sam policzy
	 * nastepny termin od biezacego zegara. */
	exit(FPM_EXIT_OK);
}
/* }}} */

void fpm_pool_cron_status(struct fpm_worker_pool_s *wp, struct fpm_pool_status_s *out) /* {{{ */
{
	struct fpm_cron_shared_s *shared = fpm_pool_cron_shared_for(wp);

	memset(out, 0, sizeof(*out));

	/* next_run NIE jest odczytywane z shared — liczymy je NA BIEZACO,
	 * dokladnie tak samo jak sam cron liczy termin swojego kolejnego
	 * przebiegu (fpm_cron_schedule_next() od "teraz"). To dziala nawet gdy
	 * shared == NULL (np. init_main jeszcze sie nie wykonal) i jest jedynym
	 * powodem, dla ktorego next_run nie wymagalo zadnego stanu w pamieci
	 * dzielonej — patrz docs/NOTES.md 3u i 3r. */
	if (wp->config->cron_parsed_schedule) {
		time_t n = fpm_cron_schedule_next(wp->config->cron_parsed_schedule, time(NULL));
		out->has_next_run = 1;
		out->next_run = (n == (time_t) -1) ? 0 : n;
	}

	if (!shared) {
		out->state = FPM_POOL_STATE_IDLE;
		return;
	}

	out->state = shared->running ? FPM_POOL_STATE_RUNNING : FPM_POOL_STATE_IDLE;
	out->last_start = shared->last_run;
	out->last_exit_code = shared->last_exit_code;
	out->has_last_exit_code = shared->has_last_exit_code;
	out->consecutive_failures = shared->consecutive_failures;
}
/* }}} */
