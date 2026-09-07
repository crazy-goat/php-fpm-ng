/* fpm-ng: pool.executor = async — EXPERIMENT (see fpm_pool_async.h, NOTES 3t).
 *
 * Model: the child performs ONE php_request_startup() (the "request container"),
 * then each FastCGI request gets its own True Async coroutine. SAPI state (SG)
 * and superglobals are switched on every coroutine switch by the fork's
 * switch handler (ZEND_COROUTINE_ADD_SWITCH_HANDLER). The remaining request
 * state (EG(symbol_table), class/function tables, included_files, ini,
 * memory_limit, max_execution_time) is SHARED — a deliberate POC limitation,
 * not an oversight. See NOTES 3t for the full list.
 *
 * For now validate() rejects the pool on every engine: the request container
 * does not yet have the isolation or guards required to serve multiple requests
 * safely. The implementation remains in the tree as material for future
 * hardening.
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

/* Engine detection: Zend/zend_async_API.h exists only in the true-async fork.
 * Upstream does not have it, so the macro remains undefined and child_main is
 * never called (validate rejects the pool). */
#if defined(__has_include)
# if __has_include("zend_async_API.h")
#  include "zend_async_API.h"
#  define FPMNG_ASYNC_ENGINE 1
# endif
#endif

/* The POC operates directly on sapi_globals (memcpy), which makes sense only in NTS. */
#if defined(FPMNG_ASYNC_ENGINE) && defined(ZTS)
# undef FPMNG_ASYNC_ENGINE
# define FPMNG_ASYNC_NO_ZTS 1
#endif

const char *const fpm_pool_async_rejects[] = {
	"pm.max_requests",			/* request counts are not tracked per process */
	"request_terminate_timeout",		/* the scoreboard does not see coroutine requests */
	"request_terminate_timeout_track_finished",
	"request_slowlog_timeout",
	"request_slowlog_trace_depth",
	"slowlog",
	"ping.",				/* ping is handled by the fpm_main.c loop, not us */
	"fiber.",				/* worker replacement after file changes lives in the Fiber scheduler */
	NULL
};

int fpm_pool_async_validate(struct fpm_worker_pool_s *wp) /* {{{ */
{
	/* The implementation is kept for a future hardening pass, but it currently
	 * shares process-wide request state without the guards used by fiber. Do not
	 * let a True Async build turn this experimental POC into a supported pool. */
	zlog(ZLOG_ALERT, "[pool %s] pool.executor = async is disabled: it is not hardened "
		"for concurrent requests; use pool.executor = classic or fiber until async parity is implemented",
		wp->config->name);
	return -1;
}
/* }}} */

#ifndef FPMNG_ASYNC_ENGINE

void fpm_pool_async_child_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	zlog(ZLOG_ALERT, "[pool %s] pool.executor = async: engine without True Async API, this should have been rejected by validate()",
		wp->config->name);
	exit(FPM_EXIT_SOFTWARE);
}
/* }}} */

#else /* FPMNG_ASYNC_ENGINE */

/* Auto-globals that must be rearmed for every request. */
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

/* State of one in-flight request. */
struct fpm_async_req_s {
	fcgi_request *req;
	sapi_globals_struct sg;			/* this coroutine's SG while off the processor */
	HashTable symbol_table;			/* this coroutine's EG(symbol_table) while off the processor */
	HashTable included_files;		/* this coroutine's EG(included_files), likewise */
	bool tables_live;			/* symbol_table/included_files initialized and not yet destroyed */
	unsigned id;
};

/* Request-container state: what main and the acceptor see when no request is on
 * the processor. Copy tables BY VALUE (the HashTable header, ~56 B): the address
 * &EG(symbol_table) does not change — only the contents below it do. The main
 * script frame holds the &EG(symbol_table) pointer
 * (zend_execute -> execute_data->symbol_table), while IS_INDIRECT entries point
 * into CV slots on this coroutine's VM stack (each has its own), so replacing
 * the contents is invisible to the frame. */
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

/* fpm_main.c replaces php_import_environment_variables with a variant that
 * reads the FastCGI environment ONLY after fpm_run() returns — that is, after
 * our child_main, which does not return. Without this replacement, $_SERVER
 * would contain only the process environment. Equivalent to the static
 * cgi_php_import_environment_variables. */
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

/* The originals from fpm_main.c cast SG(server_context) to fcgi_request*
 * without checking. It is NULL in the main/acceptor context, so write there to
 * stderr (these are only engine error messages anyway). */
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

/* --- per-coroutine state switching -------------------------------------- */

/* Rearm auto-globals ($_SERVER, $_GET, ...): their callbacks disarm themselves
 * after the first construction (php_variables.c: "don't rearm"), while we do
 * not pass through zend_activate_auto_globals() per request. Compiling/loading
 * the script rebuilds them from the CURRENT SG into the CURRENT (fresh,
 * per-coroutine) EG(symbol_table) — see zend_auto_global_check in zend_compile.c. */
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

/* Fork switch handler: is_enter=true — the coroutine enters the processor,
 * false — leaves; is_finishing — leaves permanently. Returning false removes
 * the handler. */
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
		/* Request already cleaned up in fpm_async_worker_entry() (tables_live ==
		 * false); only return to the base state here. */
		memcpy(&sapi_globals, &fpm_async_base_sg, sizeof(sapi_globals));
		coroutine->extended_data = NULL;
		efree(ctx);
		return false;
	}

	fpm_async_tables_leave(ctx);
	return true;
}
/* }}} */

/* --- one request --------------------------------------------------------- */

/* Equivalent to sapi_activate() + init_request_info() from fpm_main.c, but on
 * a FRESH copy of SG, without php_request_startup(). */
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

/* Request coroutine body. Context: ZEND_ASYNC_CURRENT_COROUTINE->extended_data. */
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

	/* Own symbol table and included_files list — as init_executor()
	 * (zend_execute_API.c) creates them for every request. Without this, two main
	 * scripts attach CVs with the same names to ONE table
	 * (zend_attach_symbol_table); the second takes over the first's values bitwise
	 * without addref and frees them underneath it — measured: SIGABRT in
	 * gc_possible_root for net.php with a socket resource in $fp. */
	zend_hash_init(&EG(symbol_table), 64, NULL, ZVAL_PTR_DTOR, 0);
	zend_hash_init(&EG(included_files), 8, NULL, NULL, 0);
	ctx->tables_live = true;
	fpm_async_superglobals_rearm();

	/* Do not rely on the compiler to create auto-globals: on an OPcache hit it
	 * does not analyze references to $_GET/$_SERVER again. Explicit
	 * initialization fills the fresh symbol table even for a cached op_array.
	 * $_REQUEST must be created after $_GET, $_POST, and $_COOKIE. */
	zend_is_auto_global_str("_GET", sizeof("_GET") - 1);
	zend_is_auto_global_str("_POST", sizeof("_POST") - 1);
	zend_is_auto_global_str("_COOKIE", sizeof("_COOKIE") - 1);
	zend_is_auto_global_str("_FILES", sizeof("_FILES") - 1);
	zend_is_auto_global_str("_SERVER", sizeof("_SERVER") - 1);
	zend_is_auto_global_str("_ENV", sizeof("_ENV") - 1);
	zend_is_auto_global_str("_REQUEST", sizeof("_REQUEST") - 1);

	/* fiber_entry in ext/async (scheduler.c) sets EG(error_reporting) from the
	 * "error_reporting" INI value instead of inheriting it — without php.ini this
	 * becomes 0 and warnings plus "Uncaught ..." disappear. Inherit the
	 * request-container value. */
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

		/* NOT php_execute_script(): in the fork it calls
		 * ZEND_ASYNC_RUN_SCHEDULER_AFTER_MAIN, which treats the current coroutine
		 * as the ending main coroutine and finalizes it (scheduler.c:
		 * async_scheduler_main_coroutine_suspend). */
		/* A coroutine Fiber has an artificial internal-function frame at its bottom
		 * (ext/async scheduler.c: fiber_entry, root_function). With non-NULL
		 * EG(current_execute_data), zend_execute() searches up the stack for the
		 * symbol table (zend_rebuild_symbol_table) and gets NULL for this frame ->
		 * SIGSEGV in zend_attach_symbol_table. The main script must attach to
		 * EG(symbol_table), so pretend the stack is empty while executing it. */
		zend_execute_data *saved_execute_data = EG(current_execute_data);
		EG(current_execute_data) = NULL;
		zend_try {
			zend_execute_scripts(ZEND_REQUIRE, NULL, 1, &file_handle);
			if (EG(exception)) {
				/* zend_execute_script in the fork skips zend_exception_error inside
				 * the coroutine (Zend/zend.c), so do it ourselves — as FPM does: fatal, 255. */
				zend_exception_error(EG(exception), E_ERROR);
			}
		} zend_catch {
			EG(exit_status) = 255;
		} zend_end_try();
		EG(current_execute_data) = saved_execute_data;

		zend_destroy_file_handle(&file_handle);
	}

	/* Headers (if nothing was output) + SAPI flush — like php_request_shutdown
	 * -> php_output_end_all. */
	zend_try {
		if (!SG(headers_sent)) {
			sapi_send_headers();
		}
		sapi_flush();
	} zend_catch {
	} zend_end_try();

	/* POC: no keep-alive at the pool level — close the connection after the
	 * response. Keep-alive would require asynchronous waiting for the next
	 * request on the same fd (fcgi_accept_request reads blocking on an open fd). */
	fcgi_request_set_keep(req, 0);
	fcgi_finish_request(req, 0);

	zlog(ZLOG_DEBUG, "[pool %s] async: request #%u done, exit_status=%d",
		fpm_async_pool_name, ctx->id, EG(exit_status));

	fpm_async_request_deactivate(ctx);
	fcgi_destroy_request(req);
	ctx->req = NULL;

	/* Like shutdown_executor(): release request global variables (object
	 * destructors run here, still in this coroutine's context) and return to the
	 * container tables. The main script has already detached its CVs. */
	zend_hash_graceful_reverse_destroy(&EG(symbol_table));
	zend_hash_destroy(&EG(included_files));
	ctx->tables_live = false;
	memcpy(&EG(symbol_table), &fpm_async_base_symbol_table, sizeof(HashTable));
	memcpy(&EG(included_files), &fpm_async_base_included_files, sizeof(HashTable));
	fpm_async_in_flight--;
	/* The rest (return to base SG, efree(ctx)) happens in the switch handler at is_finishing. */
}
/* }}} */

/* --- acceptor ------------------------------------------------------------- */

/* Wait (asynchronously) for the listening socket to become ready, then call
 * fcgi_accept_request(): accept() returns immediately, polling the new fd is
 * asynchronous in the fork (main/network.c: php_poll2 -> php_poll2_async), and
 * reading FastCGI headers is blocking, but the data is already available. */
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

/* --- child ---------------------------------------------------------------- */

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

	/* Request container: the only php_request_startup() in the process lifetime.
	 * It provides the active executor, ext/async RINIT (ZEND_ASYNC_INITIALIZE),
	 * and the memory arena. */
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
	/* max_execution_time would apply to the container, that is, the entire
	 * process lifetime — disable it. */
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

	/* The first ZEND_ASYNC_SPAWN() starts the scheduler and turns the current
	 * execution into the main coroutine (ext/async scheduler.c:
	 * async_scheduler_launch). */
	acceptor = ZEND_ASYNC_SPAWN();
	if (!acceptor) {
		zlog(ZLOG_ERROR, "[pool %s] async: cannot spawn the acceptor coroutine", wp->config->name);
		exit(FPM_EXIT_SOFTWARE);
	}
	acceptor->internal_entry = fpm_async_acceptor_entry;
	acceptor->extended_data = (void *) (intptr_t) listen_fd;

	zlog(ZLOG_NOTICE, "[pool %s] async: child %d ready, engine %s, one process, N requests in flight",
		wp->config->name, (int) getpid(), ZEND_ASYNC_API);

	/* Main coroutine: wake once per second to notice SIGTERM/SIGQUIT
	 * (fpm_signals.c -> fpm_php_soft_quit -> fcgi_terminate). Everything else
	 * happens in the scheduler, to which we yield control in SUSPEND. */
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
