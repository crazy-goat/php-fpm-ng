/* fpm-ng: shared script-execution helper. See fpm_pool_script.h. Extracted
 * from fpm_pool_supervisor.c (docs/NOTES.md 3o has the original design
 * writeup) when pool.type = cron needed the exact same "run one script
 * outside of any FastCGI request" plumbing.
 */

#include "fpm_config.h"

#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include "php.h"
#include "php_main.h"
#include "php_variables.h"
#include "SAPI.h"
#include "zend_globals.h"
#include "fopen_wrappers.h"

#include "fpm_pool_script.h"
#include "fpm_stdio.h"
#include "zlog.h"

/* Nadpisania sapi_module na czas zycia tego procesu. Bezpieczne WYLACZNIE
 * dlatego, ze proces, ktory to wola, nigdy nie wraca do petli accept FastCGI
 * (child_main "nie wraca") — nie ma innego kodu w tym procesie, ktory
 * polegalby na oryginalnych wskaznikach cgi_sapi_module. SG(server_context)
 * zostaje NULL przez caly czas, wiec oryginalne wersje tych callbackow
 * (ktore go bezwarunkowo rzutuja na fcgi_request*) by segfaultowaly. */
static size_t fpm_pool_script_ub_write(const char *str, size_t str_length) /* {{{ */
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

static char *fpm_pool_script_getenv(const char *name, size_t name_len) /* {{{ */
{
	(void) name_len;
	return getenv(name);
}
/* }}} */

static size_t fpm_pool_script_read_post(char *buffer, size_t count_bytes) /* {{{ */
{
	(void) buffer;
	(void) count_bytes;
	return 0;
}
/* }}} */

static char *fpm_pool_script_read_cookies(void) /* {{{ */
{
	return NULL;
}
/* }}} */

static void fpm_pool_script_register_server_variables(zval *track_vars_array) /* {{{ */
{
	/* Brak requestu HTTP, wiec brak PHP_SELF i innych CGI-owych zmiennych —
	 * tylko srodowisko, jak w CLI. */
	php_import_environment_variables(track_vars_array);
}
/* }}} */

void fpm_pool_script_install_sapi_overrides(void) /* {{{ */
{
	sapi_module.pre_request_init = NULL;
	sapi_module.ub_write = fpm_pool_script_ub_write;
	sapi_module.getenv = fpm_pool_script_getenv;
	sapi_module.read_post = fpm_pool_script_read_post;
	sapi_module.read_cookies = fpm_pool_script_read_cookies;
	sapi_module.register_server_variables = fpm_pool_script_register_server_variables;
}
/* }}} */

int fpm_pool_script_run(const char *pool_name, const char *script_path) /* {{{ */
{
	zend_file_handle file_handle;
	int exit_code;
	struct sigaction term_before;

	/* ZMIERZONE (docs/NOTES.md, "wdzieczne zatrzymanie"): php_request_startup()
	 * i php_request_shutdown() PODMIENIAJA dyspozycje SIGTERM procesu na wlasny
	 * handler Zenda (ZEND_SIGNALS obejmuje SIGTERM w zend_sigs[], nie tylko
	 * SIGALRM uzywany do max_execution_time) — i robia to przy KAZDYM
	 * wywolaniu, nie tylko raz. Wolajacy (pool.type = supervisor/cron) instaluje
	 * WLASNY handler SIGTERM przed pierwsza iteracja PRZED wejsciem do petli —
	 * bez przywrocenia go tutaj dziala on tylko dopoki nie skonczy sie
	 * PIERWSZA iteracja: kazda kolejna (restart = always/on-failure, wiele
	 * przebiegow w tym samym procesie) dostaje z powrotem domyslna dyspozycje
	 * i SIGTERM zabija proces natychmiast, bez szansy na wdzieczne zakonczenie
	 * i bez uzbrojenia watchdoga stop_timeout/cron.timeout. Zapamietujemy
	 * dyspozycje SPRZED php_request_startup() (a wiec TA, ktora wolajacy
	 * faktycznie chcial miec) i przywracamy ja natychmiast po kazdym miejscu,
	 * w ktorym PHP moze ja podmienic — nie znajac przy tym NIC o wolajacym
	 * (moze to byc SIG_DFL, jesli caller nie zainstalowal niczego wlasnego —
	 * przywrocenie SIG_DFL jest wtedy no-opem, wiec bezpieczne zawsze). */
	sigaction(SIGTERM, NULL, &term_before);

	SG(server_context) = NULL;
	SG(request_info).path_translated = estrdup(script_path);
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
		zlog(ZLOG_ERROR, "[pool %s] cannot start request for script '%s'", pool_name, script_path);
		efree(SG(request_info).path_translated);
		SG(request_info).path_translated = NULL;
		sigaction(SIGTERM, &term_before, NULL);
		return 255;
	}
	/* Patrz komentarz przy term_before na gorze funkcji. */
	sigaction(SIGTERM, &term_before, NULL);
	/* Jak przy '-v'/phpinfo w fpm_main.c: ustawic PO startupie, bo RINIT
	 * resetuje no_headers. Nie ma dokad wysylac naglowkow, wiec sciezka
	 * send_headers zostaje calkowicie pominieta (patrz sapi_send_headers()). */
	SG(headers_sent) = true;
	SG(request_info).no_headers = 1;

	EG(exit_status) = 0;

	zend_first_try {
		if (php_fopen_primary_script(&file_handle) == FAILURE) {
			zlog(ZLOG_ERROR, "[pool %s] cannot open script '%s'", pool_name, script_path);
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
	/* php_request_shutdown() moze podmienic dyspozycje SIGTERM tak samo jak
	 * php_request_startup() (patrz komentarz przy term_before) — przywracamy
	 * jeszcze raz, zeby okno MIEDZY iteracjami (backoff, park(), oczekiwanie
	 * na kolejny termin crona) tez mialo dzialajacy handler wolajacego. */
	sigaction(SIGTERM, &term_before, NULL);
	fpm_stdio_flush_child();

	return exit_code;
}
/* }}} */
