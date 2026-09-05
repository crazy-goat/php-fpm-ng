/* fpm-ng: pool.type = async — EKSPERYMENT (patrz fpm_pool_async.h, NOTES 3t).
 *
 * Model: dziecko robi JEDEN php_request_startup() ("request-kontener"), a potem
 * kazdy request FastCGI dostaje wlasna korutyne True Async. Stan SAPI (SG)
 * i superglobale sa przelaczane przy kazdym przelaczeniu korutyny przez
 * switch-handler forka (ZEND_COROUTINE_ADD_SWITCH_HANDLER). Reszta stanu
 * requestu (EG(symbol_table), tablice klas/funkcji, included_files, ini,
 * memory_limit, max_execution_time) jest WSPOLNA — to swiadome ograniczenie
 * POC, nie przeoczenie. Lista w NOTES 3t.
 *
 * Na silniku bez True Async API plik kompiluje sie do samego validate(),
 * ktory odrzuca pool czytelnym komunikatem.
 */

#include "fpm_config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "php.h"
#include "php_main.h"
#include "php_variables.h"
#include "SAPI.h"
#include "zend_globals.h"
#include "zend_stream.h"
#include "zend_exceptions.h"
#include "fastcgi.h"

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_async.h"
#include "fpm_stdio.h"
#include "zlog.h"

/* Wykrycie silnika: naglowek Zend/zend_async_API.h istnieje tylko w forku
 * true-async. Upstream go nie ma, wiec makro zostaje niezdefiniowane i
 * child_main nigdy nie zostanie wywolany (validate odrzuca pool). */
#if defined(__has_include)
# if __has_include("zend_async_API.h")
#  include "zend_async_API.h"
#  define FPMNG_ASYNC_ENGINE 1
# endif
#endif

/* POC operuje bezposrednio na sapi_globals (memcpy), co ma sens tylko w NTS. */
#if defined(FPMNG_ASYNC_ENGINE) && defined(ZTS)
# undef FPMNG_ASYNC_ENGINE
# define FPMNG_ASYNC_NO_ZTS 1
#endif

const char *const fpm_pool_async_rejects[] = {
	"pm.max_requests",			/* nie liczymy requestow per proces */
	"request_terminate_timeout",		/* scoreboard nie widzi requestow w korutynach */
	"request_terminate_timeout_track_finished",
	"request_slowlog_timeout",
	"request_slowlog_trace_depth",
	"slowlog",
	"ping.",				/* ping obsluguje petla fpm_main.c, nie my */
	NULL
};

int fpm_pool_async_validate(struct fpm_worker_pool_s *wp) /* {{{ */
{
#ifndef FPMNG_ASYNC_ENGINE
# ifdef FPMNG_ASYNC_NO_ZTS
	zlog(ZLOG_ALERT, "[pool %s] pool.type = async is not supported in a ZTS build (PHP %s)",
		wp->config->name, PHP_VERSION);
# else
	zlog(ZLOG_ALERT, "[pool %s] pool.type = async requires a PHP engine with the True Async API "
		"(Zend/zend_async_API.h); this binary is PHP %s without it",
		wp->config->name, PHP_VERSION);
# endif
	return -1;
#else
	/* fpm_init() biegnie PO php_module_startup() (fpm_main.c: startup() przed
	 * fpm_init()), wiec MINIT ext/async juz zarejestrowal scheduler i reaktor
	 * — mozna to sprawdzic tutaj, a nie dopiero w dziecku. */
	if (!zend_async_is_enabled()) {
		zlog(ZLOG_ALERT, "[pool %s] pool.type = async: the engine has the True Async API (%s) "
			"but no scheduler/reactor is registered — ext/async is not loaded",
			wp->config->name, ZEND_ASYNC_API);
		return -1;
	}
	if (wp->config->pm != PM_STYLE_STATIC) {
		zlog(ZLOG_ALERT, "[pool %s] pool.type = async supports only pm = static "
			"(dynamic/ondemand scale on scoreboard idle/active counters this type does not maintain)",
			wp->config->name);
		return -1;
	}
	return 0;
#endif
}
/* }}} */

#ifndef FPMNG_ASYNC_ENGINE

void fpm_pool_async_child_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	zlog(ZLOG_ALERT, "[pool %s] pool.type = async: engine without True Async API, this should have been rejected by validate()",
		wp->config->name);
	exit(FPM_EXIT_SOFTWARE);
}
/* }}} */

#else /* FPMNG_ASYNC_ENGINE */

/* Auto-globale, ktore trzeba ponownie uzbroic dla kazdego requestu. */
static const struct {
	const char *name;
	size_t len;
} fpm_async_superglobals[] = {
	{ "_SERVER",  sizeof("_SERVER") - 1 },
	{ "_GET",     sizeof("_GET") - 1 },
	{ "_POST",    sizeof("_POST") - 1 },
	{ "_COOKIE",  sizeof("_COOKIE") - 1 },
	{ "_FILES",   sizeof("_FILES") - 1 },
	{ "_ENV",     sizeof("_ENV") - 1 },
	{ "_REQUEST", sizeof("_REQUEST") - 1 },
};
#define FPM_ASYNC_NSG (sizeof(fpm_async_superglobals) / sizeof(fpm_async_superglobals[0]))

/* Stan jednego requestu w locie. */
struct fpm_async_req_s {
	fcgi_request *req;
	sapi_globals_struct sg;			/* SG tej korutyny, gdy nie jest na procesorze */
	HashTable symbol_table;			/* EG(symbol_table) tej korutyny, gdy nie jest na procesorze */
	HashTable included_files;		/* EG(included_files) tej korutyny, j.w. */
	bool tables_live;			/* symbol_table/included_files zainicjowane i jeszcze nie zniszczone */
	unsigned id;
};

/* Stan requestu-kontenera: to, co widza main i akceptor, gdy zaden request
 * nie jest na procesorze. Tablice kopiujemy PRZEZ WARTOSC (naglowek
 * HashTable, ~56 B): adres &EG(symbol_table) sie nie zmienia — zmienia sie
 * zawartosc pod nim. Ramka skryptu glownego trzyma wskaznik &EG(symbol_table)
 * (zend_execute -> execute_data->symbol_table), a wpisy IS_INDIRECT wskazuja
 * w sloty CV na stosie VM tej korutyny (kazda ma wlasny), wiec podmiana
 * zawartosci jest dla ramki niewidoczna. */
static sapi_globals_struct fpm_async_base_sg;
static HashTable fpm_async_base_symbol_table;
static HashTable fpm_async_base_included_files;
static int fpm_async_base_error_reporting;
static unsigned fpm_async_req_counter = 0;
static unsigned fpm_async_in_flight = 0;
static const char *fpm_async_pool_name = "?";

static size_t (*fpm_async_orig_ub_write)(const char *str, size_t str_length);
static void (*fpm_async_orig_flush)(void *server_context);
static void (*fpm_async_orig_import_env)(zval *array_ptr);

/* fpm_main.c podmienia php_import_environment_variables na wariant czytajacy
 * srodowisko FastCGI DOPIERO po powrocie z fpm_run() — czyli po naszym
 * child_main, ktory nie wraca. Bez tej podmiany $_SERVER mialoby tylko
 * environ procesu. Odpowiednik cgi_php_import_environment_variables (static). */
static void fpm_async_load_env_var(const char *var, unsigned int var_len, char *val, unsigned int val_len, void *arg) /* {{{ */
{
	size_t new_val_len;

	(void) var_len;
	if (sapi_module.input_filter(PARSE_SERVER, (char *) var, &val, val_len, &new_val_len)) {
		php_register_variable_safe((char *) var, val, new_val_len, (zval *) arg);
	}
}
/* }}} */

static void fpm_async_import_environment_variables(zval *array_ptr) /* {{{ */
{
	fpm_async_orig_import_env(array_ptr);
	if (SG(server_context)) {
		fcgi_loadenv((fcgi_request *) SG(server_context), fpm_async_load_env_var, array_ptr);
	}
}
/* }}} */

/* Oryginaly z fpm_main.c rzutuja SG(server_context) na fcgi_request* bez
 * sprawdzenia. W kontekscie main/akceptora jest NULL, wiec tam piszemy na
 * stderr (to i tak tylko komunikaty bledow silnika). */
static size_t fpm_pool_async_ub_write(const char *str, size_t str_length) /* {{{ */
{
	if (!SG(server_context)) {
		ssize_t n = write(STDERR_FILENO, str, str_length);
		return n < 0 ? 0 : (size_t) n;
	}
	return fpm_async_orig_ub_write(str, str_length);
}
/* }}} */

static void fpm_pool_async_flush(void *server_context) /* {{{ */
{
	if (server_context) {
		fpm_async_orig_flush(server_context);
	}
}
/* }}} */

/* --- przelaczanie stanu per korutyna ------------------------------------ */

/* Ponownie uzbraja auto-globale ($_SERVER, $_GET, ...): ich callbacki
 * rozbrajaja sie po pierwszym zbudowaniu (php_variables.c: "don't rearm"),
 * a my nie przechodzimy przez zend_activate_auto_globals() per request.
 * Kompilacja/ladowanie skryptu zbuduje je od nowa z BIEZACEGO SG do
 * BIEZACEJ (swiezej, per korutyna) EG(symbol_table) — patrz
 * zend_auto_global_check w zend_compile.c. */
static void fpm_async_superglobals_rearm(void) /* {{{ */
{
	size_t i;

	for (i = 0; i < FPM_ASYNC_NSG; i++) {
		zend_auto_global *ag = zend_hash_str_find_ptr(CG(auto_globals), fpm_async_superglobals[i].name, fpm_async_superglobals[i].len);

		if (ag) {
			ag->armed = 1;
		}
	}
}
/* }}} */

static void fpm_async_tables_enter(struct fpm_async_req_s *ctx) /* {{{ */
{
	memcpy(&sapi_globals, &ctx->sg, sizeof(sapi_globals));
	if (ctx->tables_live) {
		memcpy(&EG(symbol_table), &ctx->symbol_table, sizeof(HashTable));
		memcpy(&EG(included_files), &ctx->included_files, sizeof(HashTable));
	}
}
/* }}} */

static void fpm_async_tables_leave(struct fpm_async_req_s *ctx) /* {{{ */
{
	memcpy(&ctx->sg, &sapi_globals, sizeof(sapi_globals));
	memcpy(&sapi_globals, &fpm_async_base_sg, sizeof(sapi_globals));
	if (ctx->tables_live) {
		memcpy(&ctx->symbol_table, &EG(symbol_table), sizeof(HashTable));
		memcpy(&ctx->included_files, &EG(included_files), sizeof(HashTable));
		memcpy(&EG(symbol_table), &fpm_async_base_symbol_table, sizeof(HashTable));
		memcpy(&EG(included_files), &fpm_async_base_included_files, sizeof(HashTable));
	}
}
/* }}} */

/* Switch-handler forka: is_enter=true — korutyna wchodzi na procesor,
 * false — schodzi; is_finishing — schodzi na dobre. Zwrot false usuwa handler. */
static bool fpm_async_switch_handler(zend_coroutine_t *coroutine, bool is_enter, bool is_finishing) /* {{{ */
{
	struct fpm_async_req_s *ctx = coroutine->extended_data;

	if (!ctx) {
		return false;
	}

	if (is_enter) {
		fpm_async_tables_enter(ctx);
		return true;
	}

	if (is_finishing) {
		/* request juz posprzatany w fpm_async_worker_entry() (tables_live == false); tu tylko wracamy do bazy */
		memcpy(&sapi_globals, &fpm_async_base_sg, sizeof(sapi_globals));
		coroutine->extended_data = NULL;
		efree(ctx);
		return false;
	}

	fpm_async_tables_leave(ctx);
	return true;
}
/* }}} */

/* --- jeden request -------------------------------------------------------- */

/* Odpowiednik sapi_activate() + init_request_info() z fpm_main.c, ale na
 * SWIEZEJ kopii SG, bez php_request_startup(). */
static void fpm_async_request_activate(struct fpm_async_req_s *ctx) /* {{{ */
{
	fcgi_request *req = ctx->req;
	char *script = fcgi_getenv(req, "SCRIPT_FILENAME", sizeof("SCRIPT_FILENAME") - 1);
	char *content_length = fcgi_getenv(req, "CONTENT_LENGTH", sizeof("CONTENT_LENGTH") - 1);

	memcpy(&sapi_globals, &fpm_async_base_sg, sizeof(sapi_globals));

	SG(server_context) = req;
	memset(&SG(request_info), 0, sizeof(SG(request_info)));
	SG(request_info).path_translated = script ? estrdup(script) : NULL;
	SG(request_info).request_method = fcgi_getenv(req, "REQUEST_METHOD", sizeof("REQUEST_METHOD") - 1);
	SG(request_info).query_string = fcgi_getenv(req, "QUERY_STRING", sizeof("QUERY_STRING") - 1);
	SG(request_info).request_uri = fcgi_getenv(req, "REQUEST_URI", sizeof("REQUEST_URI") - 1);
	SG(request_info).content_type = fcgi_getenv(req, "CONTENT_TYPE", sizeof("CONTENT_TYPE") - 1);
	SG(request_info).content_length = content_length ? atol(content_length) : 0;
	SG(request_info).proto_num = 1000;
	SG(request_info).headers_only = SG(request_info).request_method && !strcmp(SG(request_info).request_method, "HEAD");
	SG(request_info).cookie_data = sapi_module.read_cookies ? sapi_module.read_cookies() : NULL;

	zend_llist_init(&SG(sapi_headers).headers, sizeof(sapi_header_struct), (llist_dtor_func_t) sapi_free_header, 0);
	SG(sapi_headers).send_default_content_type = 1;
	SG(sapi_headers).http_response_code = 200;
	SG(sapi_headers).http_status_line = NULL;
	SG(sapi_headers).mimetype = NULL;
	SG(headers_sent) = 0;
	SG(read_post_bytes) = 0;
	SG(post_read) = 0;
	SG(rfc1867_uploaded_files) = NULL;
	SG(global_request_time) = 0;
}
/* }}} */

static void fpm_async_request_deactivate(struct fpm_async_req_s *ctx) /* {{{ */
{
	zend_llist_destroy(&SG(sapi_headers).headers);
	if (SG(sapi_headers).mimetype) {
		efree(SG(sapi_headers).mimetype);
	}
	if (SG(sapi_headers).http_status_line) {
		efree(SG(sapi_headers).http_status_line);
	}
	if (SG(request_info).path_translated) {
		efree(SG(request_info).path_translated);
	}
	SG(server_context) = NULL;
	(void) ctx;
}
/* }}} */

/* Cialo korutyny requestu. Kontekst: ZEND_ASYNC_CURRENT_COROUTINE->extended_data. */
static void fpm_async_worker_entry(void) /* {{{ */
{
	zend_coroutine_t *self = ZEND_ASYNC_CURRENT_COROUTINE;
	struct fpm_async_req_s *ctx = ecalloc(1, sizeof(*ctx));
	fcgi_request *req = self->extended_data;
	zend_file_handle file_handle;

	ctx->req = req;
	ctx->id = ++fpm_async_req_counter;
	self->extended_data = ctx;
	fpm_async_in_flight++;

	fpm_async_request_activate(ctx);

	/* Wlasna tablica symboli i lista included_files — jak init_executor()
	 * (zend_execute_API.c) robi to dla kazdego requestu. Bez tego dwa skrypty
	 * glowne zaczepiaja CV pod tymi samymi nazwami w JEDNEJ tablicy
	 * (zend_attach_symbol_table), drugi przejmuje bitowo wartosci pierwszego
	 * bez addref i zwalnia je pod nim — zmierzone: SIGABRT w gc_possible_root
	 * na net.php z zasobem gniazda w $fp. */
	zend_hash_init(&EG(symbol_table), 64, NULL, ZVAL_PTR_DTOR, 0);
	zend_hash_init(&EG(included_files), 8, NULL, NULL, 0);
	ctx->tables_live = true;
	fpm_async_superglobals_rearm();

	/* Nie polegamy na kompilatorze przy tworzeniu auto-globali: przy trafieniu
	 * w opcache nie analizuje on ponownie odwolania do $_GET/$_SERVER. Jawna
	 * inicjalizacja wypelnia swieza tablice symboli takze dla op_array z cache.
	 * $_REQUEST musi powstac po $_GET, $_POST i $_COOKIE. */
	zend_is_auto_global_str("_GET", sizeof("_GET") - 1);
	zend_is_auto_global_str("_POST", sizeof("_POST") - 1);
	zend_is_auto_global_str("_COOKIE", sizeof("_COOKIE") - 1);
	zend_is_auto_global_str("_FILES", sizeof("_FILES") - 1);
	zend_is_auto_global_str("_SERVER", sizeof("_SERVER") - 1);
	zend_is_auto_global_str("_ENV", sizeof("_ENV") - 1);
	zend_is_auto_global_str("_REQUEST", sizeof("_REQUEST") - 1);

	/* fiber_entry w ext/async (scheduler.c) ustawia EG(error_reporting) z ini
	 * "error_reporting" zamiast dziedziczyc — bez php.ini daje to 0 i ostrzezenia
	 * oraz "Uncaught ..." znikaja. Dziedziczymy wartosc requestu-kontenera. */
	EG(error_reporting) = fpm_async_base_error_reporting;

	ZEND_COROUTINE_ADD_SWITCH_HANDLER(self, fpm_async_switch_handler);

	zlog(ZLOG_DEBUG, "[pool %s] async: request #%u start (%s), in flight: %u",
		fpm_async_pool_name, ctx->id, SG(request_info).request_uri ? SG(request_info).request_uri : "-", fpm_async_in_flight);

	EG(exit_status) = 0;

	if (!SG(request_info).path_translated) {
		SG(sapi_headers).http_response_code = 400;
	} else {
		zend_stream_init_filename(&file_handle, SG(request_info).path_translated);
		file_handle.primary_script = 1;

		/* NIE php_execute_script(): w forku wola ono
		 * ZEND_ASYNC_RUN_SCHEDULER_AFTER_MAIN, ktore traktuje biezaca korutyne
		 * jak konczaca sie glowna i finalizuje ja (scheduler.c:
		 * async_scheduler_main_coroutine_suspend). */
		/* Fiber korutyny ma na dnie sztuczna ramke funkcji wewnetrznej
		 * (ext/async scheduler.c: fiber_entry, root_function). zend_execute()
		 * przy niepustym EG(current_execute_data) szuka tablicy symboli w gore
		 * stosu (zend_rebuild_symbol_table) i dla takiej ramki dostaje NULL ->
		 * SIGSEGV w zend_attach_symbol_table. Skrypt glowny ma zaczepic
		 * EG(symbol_table), wiec na czas wykonania udajemy pusty stos. */
		zend_execute_data *saved_execute_data = EG(current_execute_data);
		EG(current_execute_data) = NULL;
		zend_try {
			zend_execute_scripts(ZEND_REQUIRE, NULL, 1, &file_handle);
			if (EG(exception)) {
				/* zend_execute_script w forku pomija zend_exception_error
				 * wewnatrz korutyny (Zend/zend.c), wiec robimy to sami — jak FPM: fatal, 255. */
				zend_exception_error(EG(exception), E_ERROR);
			}
		} zend_catch {
			EG(exit_status) = 255;
		} zend_end_try();
		EG(current_execute_data) = saved_execute_data;

		zend_destroy_file_handle(&file_handle);
	}

	/* Naglowki (jesli nic nie wypisano) + flush przez SAPI — jak php_request_shutdown -> php_output_end_all. */
	zend_try {
		if (!SG(headers_sent)) {
			sapi_send_headers();
		}
		sapi_flush();
	} zend_catch {
	} zend_end_try();

	/* POC: bez keep-alive po stronie poola — polaczenie zamykamy po odpowiedzi.
	 * Keep-alive wymagalby asynchronicznego czekania na kolejny request na tym
	 * samym fd (fcgi_accept_request na otwartym fd czyta blokujaco). */
	fcgi_request_set_keep(req, 0);
	fcgi_finish_request(req, 0);

	zlog(ZLOG_DEBUG, "[pool %s] async: request #%u done, exit_status=%d",
		fpm_async_pool_name, ctx->id, EG(exit_status));

	fpm_async_request_deactivate(ctx);
	fcgi_destroy_request(req);
	ctx->req = NULL;

	/* Jak shutdown_executor(): zwalniamy zmienne globalne requestu (destruktory
	 * obiektow biegna tu, jeszcze w kontekscie tej korutyny) i wracamy do
	 * tablic kontenera. Skrypt glowny juz odczepil swoje CV. */
	zend_hash_graceful_reverse_destroy(&EG(symbol_table));
	zend_hash_destroy(&EG(included_files));
	ctx->tables_live = false;
	memcpy(&EG(symbol_table), &fpm_async_base_symbol_table, sizeof(HashTable));
	memcpy(&EG(included_files), &fpm_async_base_included_files, sizeof(HashTable));
	fpm_async_in_flight--;
	/* reszta (powrot do bazowego SG, efree(ctx)) w switch-handlerze przy is_finishing */
}
/* }}} */

/* --- akceptor ------------------------------------------------------------- */

/* Czeka (asynchronicznie) na gotowosc gniazda nasluchujacego, potem
 * fcgi_accept_request(): accept() wraca od razu, poll na nowym fd jest w forku
 * asynchroniczny (main/network.c: php_poll2 -> php_poll2_async), a czytanie
 * naglowkow FastCGI jest blokujace, ale dane juz sa. */
static void fpm_async_acceptor_entry(void) /* {{{ */
{
	zend_coroutine_t *self = ZEND_ASYNC_CURRENT_COROUTINE;
	int listen_fd = (int) (intptr_t) self->extended_data;
	zend_async_poll_event_t *ev;

	self->extended_data = NULL;

	ev = ZEND_ASYNC_NEW_SOCKET_EVENT(listen_fd, ASYNC_READABLE);
	if (!ev) {
		zlog(ZLOG_ERROR, "[pool %s] async: cannot create poll event for the listening socket", fpm_async_pool_name);
		fcgi_terminate();
		return;
	}

	while (!fcgi_in_shutdown()) {
		fcgi_request *req;
		zend_coroutine_t *worker;

		ZEND_ASYNC_WAKER_NEW(self);
		zend_async_resume_when(self, &ev->base, false, zend_async_waker_callback_resolve, NULL);
		if (EG(exception) || !ZEND_ASYNC_SUSPEND()) {
			zend_async_waker_clean(self);
			if (EG(exception)) {
				zend_clear_exception();
			}
			if (fcgi_in_shutdown()) {
				break;
			}
			continue;
		}
		zend_async_waker_clean(self);

		req = fcgi_init_request(listen_fd, NULL, NULL, NULL);
		if (fcgi_accept_request(req) < 0) {
			fcgi_destroy_request(req);
			continue;
		}

		worker = ZEND_ASYNC_SPAWN();
		if (!worker) {
			zlog(ZLOG_ERROR, "[pool %s] async: spawn failed", fpm_async_pool_name);
			fcgi_finish_request(req, 1);
			fcgi_destroy_request(req);
			if (EG(exception)) {
				zend_clear_exception();
			}
			continue;
		}
		worker->internal_entry = fpm_async_worker_entry;
		worker->extended_data = req;
	}

	ZEND_ASYNC_EVENT_RELEASE(&ev->base);
}
/* }}} */

/* --- dziecko -------------------------------------------------------------- */

void fpm_pool_async_child_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	int listen_fd = fpm_globals.listening_socket;
	zend_coroutine_t *acceptor;

	fpm_async_pool_name = wp->config->name;

	fpm_async_orig_ub_write = sapi_module.ub_write;
	fpm_async_orig_flush = sapi_module.flush;
	sapi_module.ub_write = fpm_pool_async_ub_write;
	sapi_module.flush = fpm_pool_async_flush;
	fpm_async_orig_import_env = php_import_environment_variables;
	php_import_environment_variables = fpm_async_import_environment_variables;

	/* Request-kontener: jedyny php_request_startup() w zyciu procesu. Daje
	 * aktywny executor, RINIT ext/async (ZEND_ASYNC_INITIALIZE) i arene pamieci. */
	SG(server_context) = NULL;
	memset(&SG(request_info), 0, sizeof(SG(request_info)));
	SG(request_info).proto_num = 1000;
	SG(sapi_headers).http_response_code = 200;

	if (php_request_startup() == FAILURE) {
		zlog(ZLOG_ERROR, "[pool %s] async: php_request_startup() failed", wp->config->name);
		exit(FPM_EXIT_SOFTWARE);
	}
	SG(headers_sent) = 1;
	SG(request_info).no_headers = 1;
	/* max_execution_time dotyczy kontenera, czyli calego zycia procesu — wylaczamy. */
	zend_unset_timeout();

	memcpy(&fpm_async_base_sg, &sapi_globals, sizeof(sapi_globals));
	memcpy(&fpm_async_base_symbol_table, &EG(symbol_table), sizeof(HashTable));
	memcpy(&fpm_async_base_included_files, &EG(included_files), sizeof(HashTable));
	fpm_async_base_error_reporting = EG(error_reporting);

	if (!ZEND_ASYNC_IS_READY) {
		zlog(ZLOG_ERROR, "[pool %s] async: ext/async did not initialize in RINIT (state=%d)",
			wp->config->name, (int) ZEND_ASYNC_G(state));
		exit(FPM_EXIT_SOFTWARE);
	}

	/* Pierwszy ZEND_ASYNC_SPAWN() uruchamia scheduler i zamienia biezacy
	 * przebieg w korutyne glowna (ext/async scheduler.c: async_scheduler_launch). */
	acceptor = ZEND_ASYNC_SPAWN();
	if (!acceptor) {
		zlog(ZLOG_ERROR, "[pool %s] async: cannot spawn the acceptor coroutine", wp->config->name);
		exit(FPM_EXIT_SOFTWARE);
	}
	acceptor->internal_entry = fpm_async_acceptor_entry;
	acceptor->extended_data = (void *) (intptr_t) listen_fd;

	zlog(ZLOG_NOTICE, "[pool %s] async: child %d ready, engine %s, one process, N requests in flight",
		wp->config->name, (int) getpid(), ZEND_ASYNC_API);

	/* Korutyna glowna: budzi sie co sekunde, zeby zauwazyc SIGTERM/SIGQUIT
	 * (fpm_signals.c -> fpm_php_soft_quit -> fcgi_terminate). Cala reszta
	 * dzieje sie w schedulerze, do ktorego oddajemy sterowanie w SUSPEND. */
	for (;;) {
		zend_async_waker_new_with_timeout(NULL, 1000, NULL);
		if (!ZEND_ASYNC_SUSPEND() && EG(exception)) {
			zend_clear_exception();
		}
		zend_async_waker_clean(ZEND_ASYNC_CURRENT_COROUTINE);

		if (fcgi_in_shutdown()) {
			zlog(ZLOG_NOTICE, "[pool %s] async: shutdown requested, %u request(s) in flight abandoned",
				wp->config->name, fpm_async_in_flight);
			break;
		}
	}

	fpm_stdio_flush_child();
	exit(FPM_EXIT_OK);
}
/* }}} */

#endif /* FPMNG_ASYNC_ENGINE */
