/* fpm-ng: shared core for "many requests in one process" — see fpm_pool_coop.h.
 *
 * Key trick (verified in the fork POC, NOTES 3t): the address of
 * &EG(symbol_table) does not change; only the CONTENTS under it change. The main
 * script frame keeps the &EG(symbol_table) pointer, while IS_INDIRECT entries
 * point to CV slots on the VM stack of that request (each has its own), so
 * replacing the HashTable header (memcpy ~56 B) is invisible to the frame.
 * The same applies to SG (memcpy of the complete structure) and OG.
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
#include "fpm_pool_coop_session_patch.h"
#include "fpm_pool_coop_ini.h"
#include "fpm_pool_coop_statics.h"
#include "zlog.h"

const char *const fpm_coop_rejects[] = {
	"pm.max_requests",			/* requests are not counted per process */
	"request_terminate_timeout",		/* the scoreboard does not see in-flight requests */
	"request_terminate_timeout_track_finished",
	"request_slowlog_timeout",
	"request_slowlog_trace_depth",
	"slowlog",
	"ping.",				/* fpm_main.c's loop handles ping, not us */
	NULL
};

/* PHP functions removed from the function table in the child
 * (zend_disable_functions — the same mechanism as
 * php_admin_value[disable_functions] in fpm_php.c). Everything from ext/pcntl
 * that acts on the PROCESS, not the request:
 *  - signal handlers: one process-wide table (PCNTL_G(php_signal_table) and
 *    SIGG(handlers)), reset in pcntl RSHUTDOWN, which we do not run per request.
 *    Request B overwrites handler A, and the signal reaches whichever Fiber is
 *    executing a tick — no isolation also DURING the request;
 *  - pcntl_alarm: the process's SIGALRM (on aarch64 macOS this is the Zend
 *    timeout signal);
 *  - fork/exec: duplicate or replace the entire multi-request process together
 *    with its scheduler, descriptors, and in-flight requests.
 * Block, do not repair — see docs/fiber_errors.md. A call produces
 * "Call to undefined function", function_exists() returns false, so fallback
 * code keeps working. proc_open/popen/exec (fork+exec, whose child does not
 * return to the scheduler) remain available. */
static const char fpm_coop_disabled_functions[] =
	"pcntl_signal,pcntl_signal_get_handler,pcntl_signal_dispatch,pcntl_async_signals,"
	"pcntl_sigprocmask,pcntl_sigwaitinfo,pcntl_sigtimedwait,pcntl_alarm,"
	"pcntl_fork,pcntl_rfork,pcntl_forkx,pcntl_exec";

/* Request-container state: what the event loop sees when no request is on the
 * processor. We copy tables BY VALUE (the HashTable header). */
static sapi_globals_struct fpm_coop_base_sg;
static zend_output_globals fpm_coop_base_og;
static HashTable fpm_coop_base_symbol_table;
static HashTable fpm_coop_base_included_files;

/* EXPERIMENT (FPMNG_SHARED_INCLUDES=1): the loaded-file list is SHARED by the
 * process instead of per request. Reason: function and class tables are
 * process-wide, so when included_files is per request, the second request runs
 * require_once 'vendor/autoload.php' again and gets "Cannot redeclare class
 * ComposerAutoloaderInit...". A shared list makes the bootstrap one-time
 * WITHOUT worker mode and without application changes. Known cost:
 * require_once returns true instead of the file value on the second call. */
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

/* INI key value set in the pool (php_admin_value/php_value), or NULL when the
 * pool does not touch it — then the php.ini value applies. The admin-before-
 * value order matches fpm_php_apply_defines (fpm_php.c): php_values go first,
 * and php_admin_values overwrite them last. */
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

/* Whether the pool disables OPcache itself
 * (php_admin_value/php_value[opcache.enable] = 0). */
static bool fpm_coop_pool_disables_opcache(struct fpm_worker_pool_s *wp) /* {{{ */
{
	const char *value = fpm_coop_pool_ini(wp, "opcache.enable");

	return value && fpm_coop_ini_value_is_off(value);
}
/* }}} */

/* Effective pool max_execution_time: the pool value or php.ini. Parse it like
 * OnUpdateTimeout in main.c (ZEND_ATOL). */
static zend_long fpm_coop_pool_max_execution_time(struct fpm_worker_pool_s *wp) /* {{{ */
{
	const char *value = fpm_coop_pool_ini(wp, "max_execution_time");

	if (value) {
		return ZEND_ATOL(value);
	}
	return zend_ini_long("max_execution_time", sizeof("max_execution_time") - 1, 0);
}
/* }}} */

/* Effective session.auto_start of the pool: pool value first, php.ini
 * fallback otherwise -- same shape as fpm_coop_pool_max_execution_time()
 * above. session.auto_start is an ordinary ini boolean (OnUpdateBool),
 * parsed here the same way fpm_coop_pool_disables_opcache() parses
 * opcache.enable. */
static bool fpm_coop_pool_session_auto_start(struct fpm_worker_pool_s *wp) /* {{{ */
{
	const char *value = fpm_coop_pool_ini(wp, "session.auto_start");

	if (value) {
		return !fpm_coop_ini_value_is_off(value);
	}
	return zend_ini_long("session.auto_start", sizeof("session.auto_start") - 1, 0) != 0;
}
/* }}} */

/* session.auto_start = 1 is refused under pool.executor = fiber for two
 * independent reasons, neither fixed by the in-process session-lock patch
 * (fpm_pool_coop_session_patch.c):
 *
 * 1. Correctness, pre-existing and unrelated to locking: ext/session's own
 *    RINIT (php_rinit_session()) runs the auto-started session_start()
 *    itself, synchronously, BEFORE fpm_coop_req_run() rebuilds $_COOKIE
 *    for this request via zend_activate_auto_globals() (see
 *    fpm_coop_session_request_startup() vs. the auto-globals block right
 *    after it, both in this file) -- so the auto-started session can never
 *    see the request's own PHPSESSID cookie. Confirmed both by reading
 *    php_rinit_session() (session.c) and by a live measurement: two
 *    sequential requests with the identical cookie get two different,
 *    freshly-generated session ids (see the RED suite, test 10).
 * 2. Locking: the same ordering means the in-process session-lock patch's
 *    post-RINIT hook (fpm_coop_session_patch_req_apply()) has not run yet
 *    either, so the FIRST session_start() of such a request is not
 *    protected by it -- it runs through the real, unwrapped "files" module
 *    and can still deadlock the whole worker on concurrent access to the
 *    same session id (see docs/session-lock-arbiter-report.md, "The
 *    auto_start gap").
 *
 * Both reasons are structural (RINIT ordering), not bugs in this feature,
 * so this is a hard refusal, not a downgrade: nothing this project can hook
 * runs early enough to fix either one. */
static const char fpm_coop_session_auto_start_msg[] =
	"session.auto_start = 1 is not supported: (1) the auto-started session "
	"cannot see this request's own cookie (RINIT runs before $_COOKIE is "
	"rebuilt for the request, so every auto_start=1 request gets a fresh, "
	"unrelated session id -- see docs/session-lock-arbiter-report.md and "
	"the RED suite's test 10) and (2) it also runs outside the in-process "
	"session-lock patch's protection (the patch's hook does not exist yet "
	"when RINIT's auto-started session_start() runs, so concurrent access "
	"can still deadlock the worker -- see docs/session-lock-arbiter-report.md, "
	"\"The auto_start gap\"); set php_admin_value[session.auto_start] = 0 in "
	"this pool or session.auto_start = 0 in php.ini and call session_start() "
	"explicitly instead";

/* OPcache assumes one request per process: it clears the auto-global mask once
 * per request container (ZendAccelerator.c accel_activate), so a script loaded
 * from cache on the second and later requests does not receive $_SERVER/$_GET;
 * file timestamps are checked against container-start time, so a script edit is
 * never seen. Measured in 3t (fork) and 3u (upstream). */
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
	/* fpm_init() runs AFTER php_module_startup() (fpm_main.c), so Zend
	 * extensions and their INI entries are already loaded — check here, not in
	 * the child. */
	if (zend_get_extension("Zend OPcache")
		&& zend_ini_long("opcache.enable", sizeof("opcache.enable") - 1, 0)
		&& !fpm_coop_pool_disables_opcache(wp)) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor = %s: %s", wp->config->name, type_name, fpm_coop_opcache_msg);
		return -1;
	}
	/* The Zend timeout is ONE timer per process (setitimer/SIGPROF,
	 * zend_set_timeout), while this process runs N requests — one timer cannot
	 * represent N deadlines. The container calls zend_unset_timeout() anyway, so
	 * a non-zero value would be accepted and silently unenforced. Reject it, as
	 * with OPcache. */
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
	/* session.auto_start = 1 is refused: two independent reasons, neither
	 * fixed by the in-process session-lock patch -- see
	 * fpm_coop_pool_session_auto_start()/fpm_coop_session_auto_start_msg
	 * above for the full reasoning. */
	if (fpm_coop_pool_session_auto_start(wp)) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor = %s: %s", wp->config->name, type_name,
			fpm_coop_session_auto_start_msg);
		return -1;
	}
	/* fpm_conf_set_time parses through atoi(), so "-1" passes silently. */
	if (wp->config->fiber_revalidate_freq < 0) {
		zlog(ZLOG_ALERT, "[pool %s] fiber.revalidate_freq = %d: must be 0 (off) or a positive number of seconds",
			wp->config->name, wp->config->fiber_revalidate_freq);
		return -1;
	}
	return 0;
#endif
}
/* }}} */

/* --- SAPI hooks ------------------------------------------------------------ */

/* fpm_main.c replaces php_import_environment_variables with a variant that
 * reads the FastCGI environment ONLY after fpm_run() returns — that is, after
 * child_main, which does not return. Without this replacement, $_SERVER would
 * contain only the process environment. Equivalent to the static
 * cgi_php_import_environment_variables there. */
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

/* The originals from fpm_main.c cast SG(server_context) to fcgi_request*
 * without checking. It is NULL in the event-loop context, so write there to
 * stderr (these are only engine error messages anyway). */
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

/* sapi_cgi_read_post from fpm_main.c keeps a static request_body_fd, which the
 * main loop resets per request — here it would remain 0 (stdin!). Read directly
 * from the FastCGI connection. Blocking: the body is usually already buffered
 * (the gateway sends the complete request), and large POSTs are a known POC
 * limitation. */
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

/* Container error handlers: UNDEF and empty stacks — the container executes no
 * user code, so it loses nothing. The container stacks were initialized by
 * init_executor(); save their headers here so a request does not overwrite
 * them. */
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

/* --- container -------------------------------------------------------------- */

int fpm_coop_container_start(const char *pool_name) /* {{{ */
{
	int i;

	fpm_coop_name = pool_name;

	/* After applying the pool's php_admin_value (fpm_php_init_child) — effective state. */
	if (zend_get_extension("Zend OPcache") && zend_ini_long("opcache.enable", sizeof("opcache.enable") - 1, 0)) {
		zlog(ZLOG_ALERT, "[pool %s] coop: %s", pool_name, fpm_coop_opcache_msg);
		return -1;
	}
	if (zend_compile_file != compile_file) {
		/* Another compile hook (disabled OPcache leaves its own, but inactive). */
		zlog(ZLOG_NOTICE, "[pool %s] coop: zend_compile_file is hooked by an extension; "
			"anything caching compiled scripts per process will misbehave with many requests in flight", pool_name);
	}

	{
		const char *shared = getenv("FPMNG_SHARED_INCLUDES");

		if (shared && *shared == '1') {
			fpm_coop_shared_includes = true;
			zlog(ZLOG_NOTICE, "[pool %s] coop: EXPERIMENT — included_files shared per process "
				"(require_once runs once per process, bootstrap does not redeclare classes)", pool_name);
		}
	}

	/* Process-wide pcntl API — see fpm_coop_disabled_functions. We are after
	 * fpm_php_init_child (MINIT and php_admin_value[extension] are complete) and
	 * before the first request: exactly the phase in which fpm_php.c applies
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

	/* MUST run before php_request_startup() below: this captures the
	 * built-in "files" save-handler module out of ps_globals.mod before
	 * this process's first-ever RINIT can touch it (see
	 * fpm_pool_coop_session_patch.c's header comment for why that timing
	 * is itself the identification mechanism). */
	fpm_coop_session_patch_container_start();

	/* Request container: the only php_request_startup() in the process lifetime.
	 * It provides the active executor, extension RINIT, and memory arena. */
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
	/* max_execution_time would apply to the container, that is, the entire
	 * process lifetime — disable it. */
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

	/* Isolate ext/session state per request — see fpm_pool_coop_session.[ch].
	 * Called HERE because only now do we have registered INI entries and the
	 * session module (if loaded at all) after the container's own RINIT. */
	fpm_coop_session_container_start();

	fpm_coop_statics_container_start(pool_name);

	return 0;
}
/* }}} */

/* --- acceptor -------------------------------------------------------------- */

fcgi_request *fpm_coop_accept(int listen_fd, int *fd_out) /* {{{ */
{
	fcgi_request *req = fcgi_init_request(listen_fd, NULL, NULL, NULL);
	int fd;

	/* The master applies the fiber type's non-blocking socket policy before
	 * forking this child. With nothing pending we get -1/EAGAIN here, which
	 * the accept callback already handles by waiting for the next event. */
	fd = fcgi_accept_request(req);

	if (fd < 0) {
		fcgi_destroy_request(req);
		return NULL;
	}
	{
		/* On BSD/macOS an accepted socket inherits O_NONBLOCK from the
		 * listening socket it came from; Linux's accept() does not carry the
		 * listening socket's flags over to the accepted one. Harmless
		 * either way, so we always clear it, but the flag is only ever set
		 * here because of the BSD/macOS behavior. */
		int fl = fcntl(fd, F_GETFL);
		if (fl >= 0 && (fl & O_NONBLOCK)) {
			fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
		}
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

	/* Did the client close? fcgi_accept_request cannot report this without
	 * falling back to blocking accept() on the listening socket. */
	do {
		n = recv(kept_fd, &c, 1, MSG_PEEK);
	} while (n < 0 && errno == EINTR);
	/* EAGAIN and EWOULDBLOCK are equal on Linux; both are checked for
	 * portability where they differ. GCC -Wlogical-op flags the equality as
	 * redundant — leave the idiom alone (docs/c-style.md, task 013). */
#if defined(__GNUC__) && !defined(__clang__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wlogical-op"
#endif
	if (n <= 0 && !(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
		fcgi_finish_request(req, 1);
		fcgi_destroy_request(req);
		return NULL;
	}
#if defined(__GNUC__) && !defined(__clang__)
# pragma GCC diagnostic pop
#endif

	/* If reading the request from this fd fails, fcgi_accept_request closes it
	 * and falls back to accept() on the listening socket. The fiber type receives
	 * a permanently non-blocking socket from the master, so this returns -1
	 * (EAGAIN) instead of blocking, without toggling the flag again. */
	fd = fcgi_accept_request(req);

	if (fd < 0) {
		fcgi_destroy_request(req);
		return NULL;
	}
	if (fd != kept_fd) {
		/* Rare race: the kept fd failed while a new connection arrived in the
		 * same window; it was accepted from a non-blocking socket (BSD/macOS
		 * inherits O_NONBLOCK — Linux does not; Linux accept() does not copy the
		 * listening-socket flags). Clear the flag and continue normally. */
		int fl = fcntl(fd, F_GETFL);
		if (fl >= 0 && (fl & O_NONBLOCK)) {
			fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
		}
	}
	*fd_out = fd;
	return req;
}
/* }}} */

/* --- state switching ------------------------------------------------------- */

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

/* --- one request ----------------------------------------------------------- */

/* Minimal equivalent of init_request_info() from fpm_main.c:
 * SCRIPT_FILENAME as the script path, without PATH_INFO fixups,
 * security.limit_extensions, or per-directory/user INI. */
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

	/* zend_execute_scripts(), NOT php_execute_script(): the latter calls chdir
	 * to the script directory (cwd is per process, while requests are in flight
	 * concurrently) and can arm zend_set_timeout (a process-wide timer).
	 *
	 * A Fiber has an artificial internal-function frame at its bottom
	 * (zend_fibers.c: zend_fiber_function; in the fork: scheduler.c fiber_entry).
	 * With non-NULL EG(current_execute_data), zend_execute() searches up the
	 * stack for the symbol table (zend_rebuild_symbol_table), gets NULL for this
	 * frame, and crashes in zend_attach_symbol_table. The main script must attach
	 * to EG(symbol_table), so pretend the stack is empty while executing it. */
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

	/* 1. SAPI: like sapi_activate() in php_request_startup(), on fresh SG. */
	fpm_coop_request_info_init(req);
	sapi_activate();

	/* 2. Output: like php_request_startup() (output_buffering from INI). */
	php_output_activate();
	if (PG(output_buffering)) {
		php_output_start_user(NULL, PG(output_buffering) > 1 ? PG(output_buffering) : 0, PHP_OUTPUT_HANDLER_STDFLAGS);
	} else if (PG(implicit_flush)) {
		php_output_set_implicit_flush(1);
	}

	/* 3. Own symbol table, included_files, superglobals, and error handlers —
	 * as init_executor() creates them for every request. Without this, two main
	 * scripts attach CVs with the same names to ONE table
	 * (zend_attach_symbol_table); the second takes over the first's values bitwise
	 * without addref and frees them underneath it — measured in 3t: SIGABRT in
	 * gc_possible_root for a script with a socket resource in $fp. */
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

	/* ext/session: base state -> live globals, module RINIT FOR THIS request
	 * (see fpm_pool_coop_session.c — why this is safe and why it is exactly the
	 * same RINIT that the classic model calls once per request; here it is called
	 * per request despite one php_request_startup() for the container process).
	 * No-op when session is not loaded. */
	fpm_coop_session_request_startup();
	fpm_coop_session_patch_req_apply();

	/* Build $_GET/$_POST/$_COOKIE/$_FILES from CURRENT SG into the CURRENT table
	 * and arm JIT auto-globals ($_SERVER, $_ENV, $_REQUEST) for compilation —
	 * exactly what php_hash_environment() does in php_request_startup(). */
	zend_activate_auto_globals();

	/* $_SERVER/$_ENV/$_REQUEST have jit == PG(auto_globals_jit) FROZEN when
	 * php_startup_auto_globals() runs (main/php_variables.c) — once per process,
	 * in php_module_startup(), BEFORE FPM applies the pool's php_admin_value after
	 * fork. php_admin_value[auto_globals_jit] in the pool configuration changes
	 * NOTHING here: the flag was already read. With jit == 1 (the default),
	 * zend_activate_auto_globals() only ARMS the entry (armed = 1) — the actual
	 * callback runs only while COMPILING a file that uses the variable
	 * (zend_is_auto_global, called from zend_compile.c for every use of $_SERVER,
	 * etc.). With FPMNG_SHARED_INCLUDES=1, vendor is not recompiled from the
	 * second request onward, so if the entry script itself does not touch
	 * auto-globals (Laravel touches them only in phpdotenv in vendor), the callback
	 * is never called and these global variables simply do not exist —
	 * "Undefined global variable $_SERVER" followed by a TypeError on null instead
	 * of an array. Force creation here, regardless of whether THIS script compiles
	 * them. zend_is_auto_global() prevents duplicate work itself: it reads and
	 * clears the "armed" flag (see zend_auto_global_check in zend_compile.c), so
	 * when the script does compile a use of $_SERVER, the callback will not start a
	 * second time.
	 * Order does not matter: php_auto_globals_create_request reads
	 * $_GET/$_POST/$_COOKIE (created above, jit=0, so already ready), not
	 * $_SERVER/$_ENV. Per-request cost: three small tables plus one getenv pass
	 * over the environment for $_ENV — cheap compared with the whole request. */
	zend_is_auto_global(ZSTR_KNOWN(ZEND_STR_AUTOGLOBAL_SERVER));
	zend_is_auto_global(ZSTR_KNOWN(ZEND_STR_AUTOGLOBAL_ENV));
	zend_is_auto_global(ZSTR_KNOWN(ZEND_STR_AUTOGLOBAL_REQUEST));

	/* Fiber starts with EG(error_reporting) from INI (zend_fibers.c:
	 * zend_fiber_execute), not the current value; without php.ini it could differ
	 * from the container. Inherit the container value. */
	EG(error_reporting) = fpm_coop_base_error_reporting;
	EG(exit_status) = 0;

	zlog(ZLOG_DEBUG, "[pool %s] coop: request #%u start (%s), in flight: %u",
		fpm_coop_name, ctx->id, SG(request_info).request_uri ? SG(request_info).request_uri : "-", fpm_coop_in_flight_n);

	/* 4. Skrypt. */
	fpm_coop_execute(ctx);

	/* ext/session: module RSHUTDOWN FOR THIS request, BEFORE destroying the
	 * symbol_table — php_session_flush() (I/O inside RSHUTDOWN) reads $_SESSION,
	 * which must still live. A suspension during flush (for example, write() to
	 * Redis) uses the normal fpm_coop_req_leave/enter because it happens IN THIS
	 * request's CONTEXT (ctx->live is still true) — exactly the same mechanism as
	 * a suspension anywhere in the script. No-op when session is not loaded. */
	fpm_coop_session_request_shutdown();

	/* 5. End like php_request_shutdown(): global-variable destructors (still in
	 * the request context), output buffers, headers, FastCGI. */
	zend_try {
		zend_hash_graceful_reverse_destroy(&EG(symbol_table));
	} zend_end_try();
	zend_try {
		php_output_end_all();
	} zend_end_try();
	zend_try {
		php_output_deactivate();	/* send headers if they have not been sent */
	} zend_end_try();
	if (!SG(headers_sent)) {
		zend_try {
			sapi_send_headers();
		} zend_end_try();
	}

	/* ini_set()/set_time_limit() in the script writes every changed entry to
	 * EG(modified_ini_directives) (zend_alter_ini_entry_ex, Zend/zend_ini.c).
	 * WHILE the request is alive (between this point and its next entry on the
	 * processor), fpm_pool_coop_ini.c restores these entries to their baseline at
	 * EVERY processor leave (Fiber suspension) and reapplies them at every entry
	 * — see fpm_pool_coop_ini.c. The once-measured effect
	 * "Laravel ini_set('display_errors','Off') remains for the whole process" was
	 * caused PRECISELY by the absence of that mechanism; with it, other in-flight
	 * requests no longer see the change. define() remains process-wide (there is
	 * no equivalent of "restore" for constants), and that will not change — see
	 * docs/frameworks.md.
	 *
	 * But THIS request is live when we reach this point (ctx->live is still true,
	 * entered), so it has its OWN changed entries installed in
	 * EG(modified_ini_directives), exactly as if it had never left the processor.
	 * This is the only moment when they must be REALLY UNWOUND (call on_modify with
	 * the baseline value — OnUpdateTimeout disarms the max_execution_time timer,
	 * OnUpdateSaveHandler switches PS(mod) back to the baseline handler — see
	 * fpm_pool_coop_ini.c, section "What this isolation does NOT fix" for why
	 * ordinary Fiber switching does NOT call on_modify while this path DOES),
	 * because this request will never return to the processor.
	 * zend_ini_deactivate() (Zend/zend_ini.c) is exactly that operation: for ALL
	 * entries in EG(modified_ini_directives) (therefore, thanks to
	 * fpm_pool_coop_ini.c, ONLY this request's own entries) it calls on_modify per
	 * entry with stage DEACTIVATE, then destroys/clears
	 * EG(modified_ini_directives) itself. The guard "whether anything was changed"
	 * is INSIDE it (if (EG(modified_ini_directives))), so a request without
	 * ini_set/set_time_limit still pays nothing here — no allocation and no INI
	 * table walk. During the request SIGPROF can still reach another Fiber — see
	 * docs/fiber_errors.md. */
	zend_ini_deactivate();
	if (EG(timeout_seconds)) {
		/* Something else armed the timer (an extension calling zend_set_timeout). */
		zend_unset_timeout();
		EG(timeout_seconds) = 0;
	}

	/* An unread POST body would corrupt the next request on this connection. */
	if (SG(request_info).content_length > SG(read_post_bytes)) {
		fcgi_request_set_keep(req, 0);
	}
	fcgi_finish_request(req, 0);

	zlog(ZLOG_DEBUG, "[pool %s] coop: request #%u done, exit_status=%d, keep=%d",
		fpm_coop_name, ctx->id, EG(exit_status), !fcgi_is_closed(req));

	/* 6. Clean up SG like sapi_deactivate_module()/sapi_deactivate_destroy()
	 * — without sapi_module.deactivate (fpm_main.c: fcgi_finish_request is
	 * already done). */
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

	/* 7. Request tables: like shutdown_executor(). The symbol table is already
	 * destroyed (the graceful destroy above also frees arData). */
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
	fpm_coop_session_patch_req_free(ctx);
	efree(ctx);
	return req;
}
/* }}} */
