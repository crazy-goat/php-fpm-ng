/* fpm-ng: wspolny rdzen "wiele requestow w jednym procesie" — patrz fpm_pool_coop.h.
 *
 * Kluczowa sztuczka (sprawdzona w POC na forku, NOTES 3t): adres
 * &EG(symbol_table) sie nie zmienia, zmienia sie ZAWARTOSC pod nim. Ramka
 * skryptu glownego trzyma wskaznik &EG(symbol_table), a wpisy IS_INDIRECT
 * wskazuja w sloty CV na stosie VM danego requestu (kazdy ma wlasny), wiec
 * podmiana naglowka HashTable (memcpy ~56 B) jest dla ramki niewidoczna.
 * To samo z SG (memcpy calej struktury) i OG.
 */

#include "fpm_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "php.h"
#include "php_main.h"
#include "php_variables.h"
#include "php_output.h"
#include "rfc1867.h"
#include "SAPI.h"
#include "zend_globals.h"
#include "zend_stream.h"
#include "zend_exceptions.h"
#include "zend_extensions.h"
#include "zend_ini.h"
#include "zend_compile.h"		/* zend_is_auto_global(), ZEND_STR_AUTOGLOBAL_* */

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_coop.h"
#include "fpm_pool_coop_session.h"
#include "fpm_pool_coop_session_lock.h"
#include "fpm_pool_coop_session_patch.h"
#include "fpm_pool_coop_ini.h"
#include "fpm_pool_coop_statics.h"
#include "zlog.h"

const char *const fpm_coop_rejects[] = {
	"pm.max_requests",			/* nie liczymy requestow per proces */
	"request_terminate_timeout",		/* scoreboard nie widzi requestow w locie */
	"request_terminate_timeout_track_finished",
	"request_slowlog_timeout",
	"request_slowlog_trace_depth",
	"slowlog",
	"ping.",				/* ping obsluguje petla fpm_main.c, nie my */
	NULL
};

/* Funkcje PHP usuwane z tablicy funkcji w dziecku (zend_disable_functions —
 * ten sam mechanizm, co php_admin_value[disable_functions] w fpm_php.c).
 * Wszystko z ext/pcntl, co dziala na PROCESIE, a nie na requescie:
 *  - handlery sygnalow: jedna tablica na proces (PCNTL_G(php_signal_table)
 *    i SIGG(handlers)), resetowana w RSHUTDOWN pcntl, ktorego u nas nie ma
 *    per request. Request B nadpisuje handler A, a sygnal trafia w ten fiber,
 *    ktory akurat wykonuje tick — brak izolacji rowniez W TRAKCIE requestu;
 *  - pcntl_alarm: SIGALRM procesu (na aarch64 macOS to sygnal timeoutu Zend);
 *  - fork/exec: duplikuja albo podmieniaja caly wielorequestowy proces wraz ze
 *    schedulerem, deskryptorami i requestami w locie.
 * Blokada, nie naprawa — patrz docs/fiber_errors.md. Wywolanie daje
 * "Call to undefined function", function_exists() zwraca false, wiec kod z
 * fallbackiem dziala dalej. proc_open/popen/exec (fork+exec, dziecko nie
 * wraca do schedulera) zostaja. */
static const char fpm_coop_disabled_functions[] =
	"pcntl_signal,pcntl_signal_get_handler,pcntl_signal_dispatch,pcntl_async_signals,"
	"pcntl_sigprocmask,pcntl_sigwaitinfo,pcntl_sigtimedwait,pcntl_alarm,"
	"pcntl_fork,pcntl_rfork,pcntl_forkx,pcntl_exec";

/* Stan requestu-kontenera: to, co widzi petla zdarzen, gdy zaden request nie
 * jest na procesorze. Tablice kopiujemy PRZEZ WARTOSC (naglowek HashTable). */
static sapi_globals_struct fpm_coop_base_sg;
static zend_output_globals fpm_coop_base_og;
static HashTable fpm_coop_base_symbol_table;
static HashTable fpm_coop_base_included_files;

/* EKSPERYMENT (FPMNG_SHARED_INCLUDES=1): lista wczytanych plikow WSPOLNA dla
 * procesu zamiast per request. Powod: tablice funkcji i klas sa procesowe,
 * wiec gdy included_files jest per request, drugi request wykonuje
 * require_once 'vendor/autoload.php' jeszcze raz i dostaje "Cannot redeclare
 * class ComposerAutoloaderInit...". Wspolna lista czyni bootstrap
 * jednorazowym BEZ worker-mode i bez zmian w aplikacji. Znany koszt:
 * require_once przy drugim wywolaniu zwraca true zamiast wartosci pliku. */
static bool fpm_coop_shared_includes = false;
static zval fpm_coop_base_http_globals[NUM_TRACK_VARS];
static int fpm_coop_base_error_reporting;
static const char *fpm_coop_name = "?";
static unsigned fpm_coop_req_counter = 0;
static unsigned fpm_coop_in_flight_n = 0;

static size_t (*fpm_coop_orig_ub_write)(const char *str, size_t str_length);
static void (*fpm_coop_orig_flush)(void *server_context);
static void (*fpm_coop_orig_import_env)(zval *array_ptr);

const char *fpm_coop_pool_name(void) /* {{{ */
{
	return fpm_coop_name;
}
/* }}} */

unsigned fpm_coop_in_flight(void) /* {{{ */
{
	return fpm_coop_in_flight_n;
}
/* }}} */

static bool fpm_coop_ini_value_is_off(const char *value) /* {{{ */
{
	zend_string *str = zend_string_init(value, strlen(value), 0);
	bool on = zend_ini_parse_bool(str);

	zend_string_release(str);
	return !on;
}
/* }}} */

/* Wartosc klucza ini ustawiona w poolu (php_admin_value/php_value) albo NULL,
 * gdy pool jej nie rusza — wtedy obowiazuje wartosc z php.ini. Kolejnosc
 * admin-przed-value odpowiada fpm_php_apply_defines (fpm_php.c): php_values
 * idzie pierwsze, php_admin_values nadpisuje je jako ostatnie. */
static const char *fpm_coop_pool_ini(struct fpm_worker_pool_s *wp, const char *key) /* {{{ */
{
	struct key_value_s *kv;

	for (kv = wp->config->php_admin_values; kv; kv = kv->next) {
		if (!strcasecmp(kv->key, key)) {
			return kv->value;
		}
	}
	for (kv = wp->config->php_values; kv; kv = kv->next) {
		if (!strcasecmp(kv->key, key)) {
			return kv->value;
		}
	}
	return NULL;
}
/* }}} */

/* Czy pool wylacza opcache u siebie (php_admin_value/php_value[opcache.enable] = 0). */
static bool fpm_coop_pool_disables_opcache(struct fpm_worker_pool_s *wp) /* {{{ */
{
	const char *value = fpm_coop_pool_ini(wp, "opcache.enable");

	return value && fpm_coop_ini_value_is_off(value);
}
/* }}} */

/* Efektywne max_execution_time poola: wartosc z poola albo z php.ini. Parsowanie
 * jak OnUpdateTimeout w main.c (ZEND_ATOL). */
static zend_long fpm_coop_pool_max_execution_time(struct fpm_worker_pool_s *wp) /* {{{ */
{
	const char *value = fpm_coop_pool_ini(wp, "max_execution_time");

	if (value) {
		return ZEND_ATOL(value);
	}
	return zend_ini_long("max_execution_time", sizeof("max_execution_time") - 1, 0);
}
/* }}} */

/* opcache zaklada jeden request na proces: maske auto-globali zeruje raz na
 * request-kontener (ZendAccelerator.c accel_activate), wiec skrypt zaladowany
 * z cache w 2. i kolejnym requescie nie dostaje $_SERVER/$_GET; znaczniki
 * czasu plikow sprawdza wzgledem czasu startu kontenera, wiec edycja skryptu
 * nigdy nie jest widziana. Zmierzone w 3t (fork) i 3u (upstream). */
static const char fpm_coop_opcache_msg[] =
	"opcache assumes one request per process (auto-globals mask and file "
	"timestamps are reset once per request-container, see docs/NOTES.md 3u); "
	"set php_admin_value[opcache.enable] = 0 in this pool or opcache.enable = 0 in php.ini";

int fpm_coop_validate(struct fpm_worker_pool_s *wp, const char *type_name) /* {{{ */
{
#ifdef ZTS
	zlog(ZLOG_ALERT, "[pool %s] pool.executor = %s is not supported in a ZTS build (PHP %s): "
		"it swaps sapi_globals/executor_globals by value, which only works in NTS",
		wp->config->name, type_name, PHP_VERSION);
	return -1;
#else
	if (wp->config->pm != PM_STYLE_STATIC) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor = %s supports only pm = static "
			"(dynamic/ondemand scale on scoreboard idle/active counters this executor does not maintain)",
			wp->config->name, type_name);
		return -1;
	}
	/* fpm_init() biegnie PO php_module_startup() (fpm_main.c), wiec rozszerzenia
	 * Zend i ich ini sa juz zaladowane — mozna sprawdzic tutaj, nie w dziecku. */
	if (zend_get_extension("Zend OPcache")
		&& zend_ini_long("opcache.enable", sizeof("opcache.enable") - 1, 0)
		&& !fpm_coop_pool_disables_opcache(wp)) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor = %s: %s", wp->config->name, type_name, fpm_coop_opcache_msg);
		return -1;
	}
	/* Timeout Zend to JEDEN timer na proces (setitimer/SIGPROF, zend_set_timeout),
	 * a w tym procesie biegnie N requestow — jeden timer nie reprezentuje N
	 * deadline'ow. Kontener i tak robi zend_unset_timeout(), wiec niezerowa
	 * wartosc bylaby przyjeta i po cichu nieegzekwowana. Odmawiamy, tak jak
	 * przy opcache. */
	{
		zend_long timeout = fpm_coop_pool_max_execution_time(wp);

		if (timeout != 0) {
			zlog(ZLOG_ALERT, "[pool %s] pool.executor = %s: max_execution_time = " ZEND_LONG_FMT
				" is not enforced (the Zend timeout is one setitimer()/SIGPROF timer per process, "
				"and this process runs many requests at once; see docs/fiber_errors.md); "
				"set php_admin_value[max_execution_time] = 0 in this pool or max_execution_time = 0 in php.ini",
				wp->config->name, type_name, timeout);
			return -1;
		}
	}
	/* fpm_conf_set_time parsuje przez atoi(), wiec "-1" przechodzi bez slowa. */
	if (wp->config->fiber_revalidate_freq < 0) {
		zlog(ZLOG_ALERT, "[pool %s] fiber.revalidate_freq = %d: must be 0 (off) or a positive number of seconds",
			wp->config->name, wp->config->fiber_revalidate_freq);
		return -1;
	}
	return 0;
#endif
}
/* }}} */

/* --- hooki SAPI ------------------------------------------------------------ */

/* fpm_main.c podmienia php_import_environment_variables na wariant czytajacy
 * srodowisko FastCGI DOPIERO po powrocie z fpm_run() — czyli po child_main,
 * ktory nie wraca. Bez tej podmiany $_SERVER mialoby tylko environ procesu.
 * Odpowiednik cgi_php_import_environment_variables (static tam). */
static void fpm_coop_load_env_var(const char *var, unsigned int var_len, char *val, unsigned int val_len, void *arg) /* {{{ */
{
	size_t new_val_len;

	(void) var_len;
	if (sapi_module.input_filter(PARSE_SERVER, (char *) var, &val, val_len, &new_val_len)) {
		php_register_variable_safe((char *) var, val, new_val_len, (zval *) arg);
	}
}
/* }}} */

static void fpm_coop_import_environment_variables(zval *array_ptr) /* {{{ */
{
	fpm_coop_orig_import_env(array_ptr);
	if (SG(server_context)) {
		fcgi_loadenv((fcgi_request *) SG(server_context), fpm_coop_load_env_var, array_ptr);
	}
}
/* }}} */

/* Oryginaly z fpm_main.c rzutuja SG(server_context) na fcgi_request* bez
 * sprawdzenia. W kontekscie petli zdarzen jest NULL, wiec tam piszemy na
 * stderr (to i tak tylko komunikaty bledow silnika). */
static size_t fpm_coop_ub_write(const char *str, size_t str_length) /* {{{ */
{
	if (!SG(server_context)) {
		ssize_t n = write(STDERR_FILENO, str, str_length);
		return n < 0 ? 0 : (size_t) n;
	}
	return fpm_coop_orig_ub_write(str, str_length);
}
/* }}} */

static void fpm_coop_flush(void *server_context) /* {{{ */
{
	if (server_context) {
		fpm_coop_orig_flush(server_context);
	}
}
/* }}} */

/* sapi_cgi_read_post z fpm_main.c trzyma statyczny request_body_fd, ktory
 * petla glowna zeruje per request — u nas zostalby 0 (stdin!). Czytamy
 * wprost z polaczenia FastCGI. Blokujaco: cialo zwykle juz jest w buforze
 * (bramka wysyla request w calosci), a duze POST-y to znane ograniczenie POC. */
static size_t fpm_coop_read_post(char *buffer, size_t count_bytes) /* {{{ */
{
	fcgi_request *req = (fcgi_request *) SG(server_context);
	size_t read_bytes = 0;
	int64_t remaining = SG(request_info).content_length - SG(read_post_bytes);

	if (!req || remaining <= 0) {
		return 0;
	}
	if ((int64_t) count_bytes > remaining) {
		count_bytes = (size_t) remaining;
	}
	while (read_bytes < count_bytes) {
		int n = fcgi_read(req, buffer + read_bytes, (int) (count_bytes - read_bytes));

		if (n <= 0) {
			break;
		}
		read_bytes += (size_t) n;
	}
	return read_bytes;
}
/* }}} */

/* Handlery bledow kontenera: UNDEF i puste stosy — kontener nie wykonuje
 * kodu uzytkownika, wiec nic nie traci. Stosy kontenera zostaly zainicjowane
 * przez init_executor(); tu zapisujemy ich naglowki, zeby request nie
 * nadpisal ich swoimi. */
static zval fpm_coop_base_user_error_handler;
static zval fpm_coop_base_user_exception_handler;
static int fpm_coop_base_user_error_handler_error_reporting;
static zend_stack fpm_coop_base_stacks[3];
static bool fpm_coop_base_handlers_saved = false;

static void fpm_coop_base_handlers_save(void) /* {{{ */
{
	if (fpm_coop_base_handlers_saved) {
		return;
	}
	ZVAL_COPY_VALUE(&fpm_coop_base_user_error_handler, &EG(user_error_handler));
	ZVAL_COPY_VALUE(&fpm_coop_base_user_exception_handler, &EG(user_exception_handler));
	fpm_coop_base_user_error_handler_error_reporting = EG(user_error_handler_error_reporting);
	fpm_coop_base_stacks[0] = EG(user_error_handlers_error_reporting);
	fpm_coop_base_stacks[1] = EG(user_error_handlers);
	fpm_coop_base_stacks[2] = EG(user_exception_handlers);
	fpm_coop_base_handlers_saved = true;
}
/* }}} */

/* --- kontener -------------------------------------------------------------- */

int fpm_coop_container_start(const char *pool_name) /* {{{ */
{
	int i;

	fpm_coop_name = pool_name;

	/* Po zastosowaniu php_admin_value poola (fpm_php_init_child) — stan faktyczny. */
	if (zend_get_extension("Zend OPcache") && zend_ini_long("opcache.enable", sizeof("opcache.enable") - 1, 0)) {
		zlog(ZLOG_ALERT, "[pool %s] coop: %s", pool_name, fpm_coop_opcache_msg);
		return -1;
	}
	if (zend_compile_file != compile_file) {
		/* inny hook kompilacji (opcache wylaczone zostawia swoj, ale nieaktywny) */
		zlog(ZLOG_NOTICE, "[pool %s] coop: zend_compile_file is hooked by an extension; "
			"anything caching compiled scripts per process will misbehave with many requests in flight", pool_name);
	}

	{
		const char *shared = getenv("FPMNG_SHARED_INCLUDES");

		if (shared && *shared == '1') {
			fpm_coop_shared_includes = true;
			zlog(ZLOG_NOTICE, "[pool %s] coop: EKSPERYMENT — included_files wspolne dla procesu "
				"(require_once wykonuje sie raz na proces, bootstrap nie redeklaruje klas)", pool_name);
		}
	}

	/* Procesowe API pcntl — patrz fpm_coop_disabled_functions. Jestesmy po
	 * fpm_php_init_child (MINIT i php_admin_value[extension] juz za nami) i
	 * przed pierwszym requestem: dokladnie ta faza, w ktorej fpm_php.c stosuje
	 * php_admin_value[disable_functions]. */
	if (zend_get_module_started("pcntl") == SUCCESS) {
		zend_disable_functions(fpm_coop_disabled_functions);
		zlog(ZLOG_NOTICE, "[pool %s] coop: ext/pcntl is loaded, disabled its process-wide functions (%s): "
			"signal handlers, alarm, fork and exec act on the whole process, which here serves many requests at once; "
			"see docs/fiber_errors.md", pool_name, fpm_coop_disabled_functions);
	}

	fpm_coop_orig_ub_write = sapi_module.ub_write;
	fpm_coop_orig_flush = sapi_module.flush;
	sapi_module.ub_write = fpm_coop_ub_write;
	sapi_module.flush = fpm_coop_flush;
	sapi_module.read_post = fpm_coop_read_post;
	fpm_coop_orig_import_env = php_import_environment_variables;
	php_import_environment_variables = fpm_coop_import_environment_variables;

	/* MUSI byc PRZED php_request_startup() nizej: ext/session resolwuje
	 * session.save_handler na nowo przy KAZDYM RINIT (nie raz na proces) -
	 * patrz fpm_pool_coop_session_lock.c - wiec modul "files_arb" musi juz
	 * byc zarejestrowany, zanim ten proces wykona swoj PIERWSZY RINIT. */
	fpm_coop_session_lock_container_start();
	fpm_coop_session_patch_container_start();

	/* Request-kontener: jedyny php_request_startup() w zyciu procesu. Daje
	 * aktywny executor, RINIT rozszerzen i arene pamieci. */
	SG(server_context) = NULL;
	memset(&SG(request_info), 0, sizeof(SG(request_info)));
	SG(request_info).proto_num = 1000;
	SG(sapi_headers).http_response_code = 200;

	if (php_request_startup() == FAILURE) {
		zlog(ZLOG_ERROR, "[pool %s] coop: php_request_startup() failed", pool_name);
		return -1;
	}
	SG(headers_sent) = 1;
	SG(request_info).no_headers = 1;
	/* max_execution_time dotyczylby kontenera, czyli calego zycia procesu — wylaczamy. */
	zend_unset_timeout();

	memcpy(&fpm_coop_base_sg, &sapi_globals, sizeof(sapi_globals));
	memcpy(&fpm_coop_base_og, &output_globals, sizeof(output_globals));
	memcpy(&fpm_coop_base_symbol_table, &EG(symbol_table), sizeof(HashTable));
	memcpy(&fpm_coop_base_included_files, &EG(included_files), sizeof(HashTable));
	for (i = 0; i < NUM_TRACK_VARS; i++) {
		ZVAL_COPY_VALUE(&fpm_coop_base_http_globals[i], &PG(http_globals)[i]);
	}
	fpm_coop_base_error_reporting = EG(error_reporting);
	fpm_coop_base_handlers_save();

	/* Izolacja stanu ext/session per request — patrz fpm_pool_coop_session.[ch].
	 * Wolane TU, bo dopiero teraz mamy zarejestrowane wpisy ini i modul
	 * session (jesli w ogole zaladowany) po wlasnym RINIT kontenera. */
	fpm_coop_session_container_start();

	fpm_coop_statics_container_start(pool_name);

	return 0;
}
/* }}} */

/* --- akceptor -------------------------------------------------------------- */

fcgi_request *fpm_coop_accept(int listen_fd, int *fd_out) /* {{{ */
{
	fcgi_request *req = fcgi_init_request(listen_fd, NULL, NULL, NULL);
	int fd = fcgi_accept_request(req);

	if (fd < 0) {
		fcgi_destroy_request(req);
		return NULL;
	}
	*fd_out = fd;
	return req;
}
/* }}} */

fcgi_request *fpm_coop_accept_kept(fcgi_request *req, int *fd_out) /* {{{ */
{
	char c;
	int kept_fd = *fd_out;
	int fd;
	ssize_t n;
	int listen_flags = -1;

	/* Klient zamknal? fcgi_accept_request nie ma jak tego zglosic bez
	 * przejscia na blokujacy accept() na gniezdzie nasluchujacym. */
	do {
		n = recv(kept_fd, &c, 1, MSG_PEEK);
	} while (n < 0 && errno == EINTR);
	if (n <= 0 && !(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
		fcgi_finish_request(req, 1);
		fcgi_destroy_request(req);
		return NULL;
	}

	/* Gdy odczyt requestu z tego fd sie nie uda, fcgi_accept_request zamyka
	 * go i przechodzi do accept() na gniezdzie nasluchujacym — blokujaco.
	 * Na czas tego wywolania robimy gniazdo nasluchujace nieblokujacym,
	 * zeby zamiast zawisnac dostac -1 (EAGAIN). */
	listen_flags = fcntl(fpm_globals.listening_socket, F_GETFL);
	if (listen_flags >= 0) {
		fcntl(fpm_globals.listening_socket, F_SETFL, listen_flags | O_NONBLOCK);
	}
	fd = fcgi_accept_request(req);
	if (listen_flags >= 0) {
		fcntl(fpm_globals.listening_socket, F_SETFL, listen_flags);
	}

	if (fd < 0) {
		fcgi_destroy_request(req);
		return NULL;
	}
	if (fd != kept_fd) {
		/* Rzadki wyscig: kept fd padl, a w tym samym oknie przyszlo nowe
		 * polaczenie; przyjete z nieblokujacego gniazda (na BSD/macOS
		 * dziedziczy O_NONBLOCK). Zdejmujemy flage, dalej zwyczajnie. */
		int fl = fcntl(fd, F_GETFL);
		if (fl >= 0 && (fl & O_NONBLOCK)) {
			fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
		}
	}
	*fd_out = fd;
	return req;
}
/* }}} */

/* --- przelaczanie stanu ---------------------------------------------------- */

struct fpm_coop_req_s *fpm_coop_req_new(fcgi_request *req, int fd) /* {{{ */
{
	struct fpm_coop_req_s *ctx = ecalloc(1, sizeof(*ctx));

	ctx->req = req;
	ctx->fd = fd;
	ctx->id = ++fpm_coop_req_counter;
	memcpy(&ctx->sg, &fpm_coop_base_sg, sizeof(sapi_globals));
	memcpy(&ctx->og, &fpm_coop_base_og, sizeof(output_globals));
	fpm_coop_in_flight_n++;
	return ctx;
}
/* }}} */

void fpm_coop_req_enter(struct fpm_coop_req_s *ctx) /* {{{ */
{
	int i;

	memcpy(&sapi_globals, &ctx->sg, sizeof(sapi_globals));
	memcpy(&output_globals, &ctx->og, sizeof(output_globals));
	if (ctx->live) {
		memcpy(&EG(symbol_table), &ctx->symbol_table, sizeof(HashTable));
		if (!fpm_coop_shared_includes) {
			memcpy(&EG(included_files), &ctx->included_files, sizeof(HashTable));
		}
		for (i = 0; i < NUM_TRACK_VARS; i++) {
			ZVAL_COPY_VALUE(&PG(http_globals)[i], &ctx->http_globals[i]);
		}
		ZVAL_COPY_VALUE(&EG(user_error_handler), &ctx->user_error_handler);
		ZVAL_COPY_VALUE(&EG(user_exception_handler), &ctx->user_exception_handler);
		EG(user_error_handler_error_reporting) = ctx->user_error_handler_error_reporting;
		EG(user_error_handlers_error_reporting) = ctx->user_error_handlers_error_reporting;
		EG(user_error_handlers) = ctx->user_error_handlers;
		EG(user_exception_handlers) = ctx->user_exception_handlers;
		fpm_coop_session_req_enter(ctx);
		fpm_coop_session_lock_req_enter(ctx);
		fpm_coop_session_patch_req_enter(ctx);
		fpm_coop_ini_req_enter(ctx);
		fpm_coop_statics_req_enter(ctx);
	}
}
/* }}} */

static void fpm_coop_base_tables_restore(void) /* {{{ */
{
	int i;

	memcpy(&EG(symbol_table), &fpm_coop_base_symbol_table, sizeof(HashTable));
	if (!fpm_coop_shared_includes) {
		memcpy(&EG(included_files), &fpm_coop_base_included_files, sizeof(HashTable));
	}
	for (i = 0; i < NUM_TRACK_VARS; i++) {
		ZVAL_COPY_VALUE(&PG(http_globals)[i], &fpm_coop_base_http_globals[i]);
	}
	ZVAL_COPY_VALUE(&EG(user_error_handler), &fpm_coop_base_user_error_handler);
	ZVAL_COPY_VALUE(&EG(user_exception_handler), &fpm_coop_base_user_exception_handler);
	EG(user_error_handler_error_reporting) = fpm_coop_base_user_error_handler_error_reporting;
	EG(user_error_handlers_error_reporting) = fpm_coop_base_stacks[0];
	EG(user_error_handlers) = fpm_coop_base_stacks[1];
	EG(user_exception_handlers) = fpm_coop_base_stacks[2];
	fpm_coop_session_base_restore();
}
/* }}} */

void fpm_coop_req_leave(struct fpm_coop_req_s *ctx) /* {{{ */
{
	int i;

	memcpy(&ctx->sg, &sapi_globals, sizeof(sapi_globals));
	memcpy(&sapi_globals, &fpm_coop_base_sg, sizeof(sapi_globals));
	memcpy(&ctx->og, &output_globals, sizeof(output_globals));
	memcpy(&output_globals, &fpm_coop_base_og, sizeof(output_globals));
	if (ctx->live) {
		memcpy(&ctx->symbol_table, &EG(symbol_table), sizeof(HashTable));
		if (!fpm_coop_shared_includes) {
			memcpy(&ctx->included_files, &EG(included_files), sizeof(HashTable));
		}
		for (i = 0; i < NUM_TRACK_VARS; i++) {
			ZVAL_COPY_VALUE(&ctx->http_globals[i], &PG(http_globals)[i]);
		}
		ZVAL_COPY_VALUE(&ctx->user_error_handler, &EG(user_error_handler));
		ZVAL_COPY_VALUE(&ctx->user_exception_handler, &EG(user_exception_handler));
		ctx->user_error_handler_error_reporting = EG(user_error_handler_error_reporting);
		ctx->user_error_handlers_error_reporting = EG(user_error_handlers_error_reporting);
		ctx->user_error_handlers = EG(user_error_handlers);
		ctx->user_exception_handlers = EG(user_exception_handlers);
		fpm_coop_session_req_save(ctx);
		fpm_coop_ini_req_leave(ctx);
		fpm_coop_statics_req_leave(ctx);
		fpm_coop_base_tables_restore();
	}
}
/* }}} */

/* --- jeden request ---------------------------------------------------------- */

/* Odpowiednik init_request_info() z fpm_main.c w wersji minimalnej:
 * SCRIPT_FILENAME jako sciezka skryptu, bez fixupow PATH_INFO, bez
 * security.limit_extensions, bez ini per katalog/uzytkownik. */
static void fpm_coop_request_info_init(fcgi_request *req) /* {{{ */
{
	char *script = fcgi_getenv(req, "SCRIPT_FILENAME", sizeof("SCRIPT_FILENAME") - 1);
	char *content_length = fcgi_getenv(req, "CONTENT_LENGTH", sizeof("CONTENT_LENGTH") - 1);

	SG(server_context) = req;
	memset(&SG(request_info), 0, sizeof(SG(request_info)));
	SG(request_info).path_translated = script ? estrdup(script) : NULL;
	SG(request_info).request_method = fcgi_getenv(req, "REQUEST_METHOD", sizeof("REQUEST_METHOD") - 1);
	SG(request_info).query_string = fcgi_getenv(req, "QUERY_STRING", sizeof("QUERY_STRING") - 1);
	SG(request_info).request_uri = fcgi_getenv(req, "REQUEST_URI", sizeof("REQUEST_URI") - 1);
	SG(request_info).content_type = fcgi_getenv(req, "CONTENT_TYPE", sizeof("CONTENT_TYPE") - 1);
	SG(request_info).content_length = content_length ? atol(content_length) : 0;
	SG(request_info).proto_num = 1000;
	SG(sapi_headers).http_response_code = 200;
}
/* }}} */

static void fpm_coop_execute(struct fpm_coop_req_s *ctx) /* {{{ */
{
	zend_file_handle file_handle;
	zend_execute_data *saved_execute_data;

	if (!SG(request_info).path_translated) {
		SG(sapi_headers).http_response_code = 404;
		PUTS("File not found.\n");
		return;
	}

	zend_stream_init_filename(&file_handle, SG(request_info).path_translated);
	file_handle.primary_script = 1;

	/* zend_execute_scripts(), NIE php_execute_script(): to drugie robi chdir
	 * do katalogu skryptu (cwd jest per proces, a requesty sa w locie
	 * rownolegle) i potrafi uzbroic zend_set_timeout (timer per proces).
	 *
	 * Fiber ma na dnie sztuczna ramke funkcji wewnetrznej (zend_fibers.c:
	 * zend_fiber_function; w forku: scheduler.c fiber_entry). zend_execute()
	 * przy niepustym EG(current_execute_data) szuka tablicy symboli w gore
	 * stosu (zend_rebuild_symbol_table), dla takiej ramki dostaje NULL i
	 * pada w zend_attach_symbol_table. Skrypt glowny ma zaczepic
	 * EG(symbol_table), wiec na czas wykonania udajemy pusty stos. */
	saved_execute_data = EG(current_execute_data);
	EG(current_execute_data) = NULL;
	zend_try {
		zend_execute_scripts(ZEND_REQUIRE, NULL, 1, &file_handle);
		if (EG(exception)) {
			zend_exception_error(EG(exception), E_ERROR);
		}
	} zend_catch {
		EG(exit_status) = 255;
	} zend_end_try();
	if (EG(exception)) {
		zend_clear_exception();
	}
	EG(current_execute_data) = saved_execute_data;

	zend_destroy_file_handle(&file_handle);
	(void) ctx;
}
/* }}} */

void fpm_coop_req_run(struct fpm_coop_req_s *ctx) /* {{{ */
{
	fcgi_request *req = ctx->req;
	int i;

	/* 1. SAPI: jak sapi_activate() w php_request_startup(), na swiezym SG. */
	fpm_coop_request_info_init(req);
	sapi_activate();

	/* 2. Wyjscie: jak php_request_startup() (output_buffering z ini). */
	php_output_activate();
	if (PG(output_buffering)) {
		php_output_start_user(NULL, PG(output_buffering) > 1 ? PG(output_buffering) : 0, PHP_OUTPUT_HANDLER_STDFLAGS);
	} else if (PG(implicit_flush)) {
		php_output_set_implicit_flush(1);
	}

	/* 3. Wlasna tablica symboli, included_files, superglobale i handlery
	 * bledow — jak init_executor() robi to dla kazdego requestu. Bez tego dwa
	 * skrypty glowne zaczepiaja CV pod tymi samymi nazwami w JEDNEJ tablicy
	 * (zend_attach_symbol_table), drugi przejmuje bitowo wartosci pierwszego
	 * bez addref i zwalnia je pod nim — zmierzone w 3t: SIGABRT w
	 * gc_possible_root na skrypcie z zasobem gniazda w $fp. */
	zend_hash_init(&EG(symbol_table), 64, NULL, ZVAL_PTR_DTOR, 0);
	if (!fpm_coop_shared_includes) {
		zend_hash_init(&EG(included_files), 8, NULL, NULL, 0);
	}
	for (i = 0; i < NUM_TRACK_VARS; i++) {
		ZVAL_UNDEF(&PG(http_globals)[i]);
	}
	ZVAL_UNDEF(&EG(user_error_handler));
	ZVAL_UNDEF(&EG(user_exception_handler));
	EG(user_error_handler_error_reporting) = E_ALL;
	zend_stack_init(&EG(user_error_handlers_error_reporting), sizeof(int));
	zend_stack_init(&EG(user_error_handlers), sizeof(zval));
	zend_stack_init(&EG(user_exception_handlers), sizeof(zval));
	ctx->live = true;

	/* ext/session: stan bazowy -> zywe globale, RINIT modulu NA TYM requescie
	 * (patrz fpm_pool_coop_session.c — dlaczego to jest bezpieczne i dlaczego
	 * to jest dokladnie ten sam RINIT, ktorego klasyczny model wola raz na
	 * request; tu wolany per request mimo jednego php_request_startup() na
	 * proces kontenera). No-op, gdy session nie jest zaladowane. */
	fpm_coop_session_request_startup();
	fpm_coop_session_patch_req_apply();

	/* Buduje $_GET/$_POST/$_COOKIE/$_FILES z BIEZACEGO SG do BIEZACEJ tablicy
	 * i uzbraja JIT-owe ($_SERVER, $_ENV, $_REQUEST) na czas kompilacji —
	 * dokladnie to, co php_hash_environment() w php_request_startup(). */
	zend_activate_auto_globals();

	/* $_SERVER/$_ENV/$_REQUEST maja jit == PG(auto_globals_jit) ZAMROZONE
	 * w chwili php_startup_auto_globals() (main/php_variables.c) — to jest
	 * jeden raz na proces, w php_module_startup(), PRZED tym jak FPM w ogole
	 * stosuje php_admin_value poola po forku. php_admin_value[auto_globals_jit]
	 * w konfiguracji poola NIC tu nie zmienia: flaga jest juz przeczytana.
	 * Z jit == 1 (domyslne) zend_activate_auto_globals() tylko UZBRAJA wpis
	 * (armed = 1) — realny callback odpala dopiero przy KOMPILACJI pliku,
	 * ktory uzywa tej zmiennej (zend_is_auto_global, wolane z zend_compile.c
	 * przy kazdym uzyciu $_SERVER itp.). Przy FPMNG_SHARED_INCLUDES=1 vendor
	 * nie jest rekompilowany od 2. requestu, wiec jesli skrypt wejsciowy sam
	 * nie dotyka autoglobali (Laravel: dotyka ich dopiero phpdotenv w
	 * vendorze), callback nigdy sie nie wola i te zmienne globalne po prostu
	 * nie istnieja — "Undefined global variable $_SERVER" i dalej TypeError
	 * na null zamiast array. Wymuszamy wiec utworzenie tu, niezaleznie od
	 * tego, czy TEN skrypt je kompiluje. zend_is_auto_global() sam pilnuje,
	 * zeby nie zrobic podwojnej roboty: czyta i zeruje flage "armed" (patrz
	 * zend_auto_global_check w zend_compile.c), wiec gdy skrypt i tak
	 * skompiluje uzycie $_SERVER, callback juz nie wystartuje drugi raz.
	 * Kolejnosc bez znaczenia: php_auto_globals_create_request czyta
	 * $_GET/$_POST/$_COOKIE (utworzone wyzej, jit=0, wiec juz gotowe), nie
	 * $_SERVER/$_ENV. Koszt per request: trzy male tablice + jeden getenv-owy
	 * przebieg po environ dla $_ENV — tanie wobec ceny calego requestu. */
	zend_is_auto_global(ZSTR_KNOWN(ZEND_STR_AUTOGLOBAL_SERVER));
	zend_is_auto_global(ZSTR_KNOWN(ZEND_STR_AUTOGLOBAL_ENV));
	zend_is_auto_global(ZSTR_KNOWN(ZEND_STR_AUTOGLOBAL_REQUEST));

	/* Fiber startuje z EG(error_reporting) z ini (zend_fibers.c:
	 * zend_fiber_execute), nie z wartosci biezacej; bez php.ini to moze byc
	 * cos innego niz w kontenerze. Dziedziczymy wartosc kontenera. */
	EG(error_reporting) = fpm_coop_base_error_reporting;
	EG(exit_status) = 0;

	zlog(ZLOG_DEBUG, "[pool %s] coop: request #%u start (%s), in flight: %u",
		fpm_coop_name, ctx->id, SG(request_info).request_uri ? SG(request_info).request_uri : "-", fpm_coop_in_flight_n);

	/* 4. Skrypt. */
	fpm_coop_execute(ctx);

	/* ext/session: RSHUTDOWN modulu NA TYM requescie, PRZED zniszczeniem
	 * symbol_table — php_session_flush() (I/O, wewnatrz RSHUTDOWN) czyta
	 * $_SESSION, ktora musi jeszcze zyc. Zawieszenie w trakcie flush (np. na
	 * write() do Redisa) przelacza sie normalnym fpm_coop_req_leave/enter,
	 * bo dzieje sie W KONTEKSCIE tego requestu (ctx->live wciaz true) —
	 * dokladnie ten sam mechanizm co zawieszenie gdziekolwiek w skrypcie.
	 * No-op, gdy session nie jest zaladowane. */
	fpm_coop_session_request_shutdown();

	/* 5. Koniec jak php_request_shutdown(): destruktory zmiennych globalnych
	 * (jeszcze w kontekscie requestu), bufory wyjscia, naglowki, FastCGI. */
	zend_try {
		zend_hash_graceful_reverse_destroy(&EG(symbol_table));
	} zend_end_try();
	zend_try {
		php_output_end_all();
	} zend_end_try();
	zend_try {
		php_output_deactivate();	/* wysyla naglowki, jesli jeszcze nie */
	} zend_end_try();
	if (!SG(headers_sent)) {
		zend_try {
			sapi_send_headers();
		} zend_end_try();
	}

	/* ini_set()/set_time_limit() w skrypcie zapisuje kazdy zmieniony wpis do
	 * EG(modified_ini_directives) (zend_alter_ini_entry_ex, Zend/zend_ini.c).
	 * DOPOKI request trwa (miedzy tym miejscem a jego pierwszym wejsciem na
	 * procesor), fpm_pool_coop_ini.c przywraca te wpisy do wartosci bazowej
	 * przy KAZDYM zejsciu z procesora (zawieszenie fibera) i naklada je z
	 * powrotem przy kazdym wejsciu — patrz fpm_pool_coop_ini.c. Zmierzony
	 * kiedys skutek "Laravel ini_set('display_errors','Off') zostaje dla
	 * calego procesu" byl objawem WLASNIE braku tamtego mechanizmu; z nim
	 * inne requesty w locie juz nie widza tej zmiany. define() nadal zostaje
	 * procesowe (nie ma odpowiednika "restore" dla stalych) i to sie nie
	 * zmieni — patrz docs/frameworks.md.
	 *
	 * Ale TEN request, w chwili gdy tu dochodzimy, jest live (ctx->live
	 * jeszcze true, entered) — a wiec ma zainstalowane WLASNE zmienione
	 * wpisy w EG(modified_ini_directives), dokladnie tak, jakby nigdy nie
	 * zszedl z procesora. To jest jedyny moment, w ktorym trzeba je ODWIKLAC
	 * NAPRAWDE (wywolac on_modify z wartoscia bazowa — OnUpdateTimeout
	 * rozbraja timer max_execution_time, OnUpdateSaveHandler przestawia
	 * PS(mod) z powrotem na bazowy handler — patrz fpm_pool_coop_ini.c,
	 * sekcja "Czego ta izolacja NIE naprawia" po to, dlaczego przy zwyklym
	 * przelaczeniu fibera NIE wolamy on_modify, a tu TAK), bo ten request
	 * juz nigdy nie wroci na procesor. zend_ini_deactivate() (Zend/zend_ini.c)
	 * to dokladnie ta operacja: dla WSZYSTKICH wpisow w
	 * EG(modified_ini_directives) (czyli, dzieki fpm_pool_coop_ini.c,
	 * WYLACZNIE wlasnych wpisow TEGO requestu) woła on_modify per wpis, stage
	 * DEACTIVATE, i sama niszczy/zeruje EG(modified_ini_directives). Straznik
	 * "czy w ogole cos bylo zmienione" jest WEWNATRZ niej (if
	 * (EG(modified_ini_directives))), wiec request bez ini_set/set_time_limit
	 * w skrypcie nadal nie placi tu nic — ani alokacji, ani przeszukania
	 * tablicy ini. W trakcie requestu SIGPROF nadal moze trafic w cudzy
	 * fiber — patrz docs/fiber_errors.md. */
	zend_ini_deactivate();
	if (EG(timeout_seconds)) {
		/* cokolwiek innego uzbroilo timer (rozszerzenie wolajace zend_set_timeout) */
		zend_unset_timeout();
		EG(timeout_seconds) = 0;
	}

	/* Nieodczytane cialo POST zepsuloby nastepny request na tym polaczeniu. */
	if (SG(request_info).content_length > SG(read_post_bytes)) {
		fcgi_request_set_keep(req, 0);
	}
	fcgi_finish_request(req, 0);

	zlog(ZLOG_DEBUG, "[pool %s] coop: request #%u done, exit_status=%d, keep=%d",
		fpm_coop_name, ctx->id, EG(exit_status), !fcgi_is_closed(req));

	/* 6. Sprzatanie SG jak sapi_deactivate_module()/sapi_deactivate_destroy()
	 * — bez sapi_module.deactivate (fpm_main.c: fcgi_finish_request juz zrobione). */
	zend_llist_destroy(&SG(sapi_headers).headers);
	if (SG(request_info).auth_user) {
		efree(SG(request_info).auth_user);
	}
	if (SG(request_info).auth_password) {
		efree(SG(request_info).auth_password);
	}
	if (SG(request_info).auth_digest) {
		efree(SG(request_info).auth_digest);
	}
	if (SG(request_info).content_type_dup) {
		efree(SG(request_info).content_type_dup);
	}
	if (SG(request_info).current_user) {
		efree(SG(request_info).current_user);
	}
	if (SG(request_info).request_body) {
		php_stream_close(SG(request_info).request_body);
		SG(request_info).request_body = NULL;
	}
	if (SG(rfc1867_uploaded_files)) {
		destroy_uploaded_files_hash();
	}
	if (SG(sapi_headers).mimetype) {
		efree(SG(sapi_headers).mimetype);
		SG(sapi_headers).mimetype = NULL;
	}
	if (SG(sapi_headers).http_status_line) {
		efree(SG(sapi_headers).http_status_line);
		SG(sapi_headers).http_status_line = NULL;
	}
	if (SG(request_info).path_translated) {
		efree(SG(request_info).path_translated);
		SG(request_info).path_translated = NULL;
	}
	SG(server_context) = NULL;

	/* 7. Tablice requestu: jak shutdown_executor(). Symbol table juz
	 * zniszczona (graceful destroy wyzej zwalnia tez arData). */
	if (!fpm_coop_shared_includes) {
		zend_hash_destroy(&EG(included_files));
	}
	for (i = 0; i < NUM_TRACK_VARS; i++) {
		zval_ptr_dtor(&PG(http_globals)[i]);
	}
	if (Z_TYPE(EG(user_error_handler)) != IS_UNDEF) {
		zval_ptr_dtor(&EG(user_error_handler));
	}
	if (Z_TYPE(EG(user_exception_handler)) != IS_UNDEF) {
		zval_ptr_dtor(&EG(user_exception_handler));
	}
	zend_stack_clean(&EG(user_error_handlers_error_reporting), NULL, 1);
	zend_stack_clean(&EG(user_error_handlers), (void (*)(void *)) ZVAL_PTR_DTOR, 1);
	zend_stack_clean(&EG(user_exception_handlers), (void (*)(void *)) ZVAL_PTR_DTOR, 1);
	zend_stack_destroy(&EG(user_error_handlers_error_reporting));
	zend_stack_destroy(&EG(user_error_handlers));
	zend_stack_destroy(&EG(user_exception_handlers));
	if (EG(exception)) {
		zend_clear_exception();
	}

	ctx->live = false;
	fpm_coop_base_tables_restore();
	fpm_coop_in_flight_n--;
}
/* }}} */

fcgi_request *fpm_coop_req_free(struct fpm_coop_req_s *ctx) /* {{{ */
{
	fcgi_request *req = ctx->req;

	fpm_coop_ini_req_free(ctx);
	fpm_coop_statics_req_free(ctx);
	fpm_coop_session_lock_req_free(ctx);
	fpm_coop_session_patch_req_free(ctx);
	efree(ctx);
	return req;
}
/* }}} */
