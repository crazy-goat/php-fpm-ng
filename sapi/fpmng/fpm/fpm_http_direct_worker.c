/* fpm-ng: worker-mode HTTP-direct — experimental POC, task 073.
 *
 * pool.type = http-direct (fpm_http_direct.c) runs one script per request from
 * inside an evhttp callback: evhttp_set_gencb (fpm_http_direct.c:508) fires
 * under event_base_dispatch (:518) and php_execute_script runs there (:431).
 * pool.executor = worker inverts that ownership on the same transport: the
 * worker boots ONE script for its whole lifetime and that script pumps the
 * libevent base itself through fpmng_worker_loop(). It is an executor rather
 * than a second pool.type for the same reason "fiber" is one — the listener,
 * the master bookkeeping and the configuration are unchanged and only the
 * child's execution model differs (fpm_pool_type.c, fpm_http_direct_worker).
 *
 * The inversion is forced, not stylistic. A userland event loop (Revolt, and
 * therefore amphp) suspends by driving the loop, so on the classic layout its
 * driver would call event_base_loop() on a base that is already looping.
 * libevent refuses that: measured on the test box with libevent
 * 2.1.12-stable, a nested event_base_loop(base, EVLOOP_ONCE|EVLOOP_NONBLOCK)
 * returns -1 and warns "event_base_loop: reentrant invocation. Only one
 * event_base_loop can run on each event_base at once." Userland `await` would
 * therefore fail outright, not merely run late.
 *
 * This file deliberately knows nothing about Revolt or amphp. It exposes
 * libevent primitives (fd/timer watchers, one loop iteration) plus a request
 * queue; the event-loop driver is userland PHP, see
 * examples/http-direct-worker/ and docs/http-direct-revolt-integration.md.
 *
 * POC limits, all documented rather than worked around: no per-request
 * isolation (one php_request_startup per worker), so `echo` belongs to the
 * worker (it goes to stderr) and a handler returns its body instead; no TLS;
 * no streaming; no per-request scoreboard accounting.
 */
#include "fpm_config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/keyvalq_struct.h>
#include <event2/buffer.h>

#include "php.h"
#include "php_main.h"
#include "php_ini.h"
#include "php_variables.h"
#include "php_streams.h"
#include "zend_API.h"
#include "zend_exceptions.h"
#include "zend_ini.h"
#include "SAPI.h"
#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_http_direct_worker.h"
#include "fpm_php.h"
#include "fpm_request.h"
#include "fpm_stdio.h"
#include "zlog.h"

#define FPM_WORKER_HEADERS_MAX (64 * 1024)
#define FPM_WORKER_BODY_MAX (8 * 1024 * 1024)
/* Bounds the memory a client burst can pin in accepted-but-unanswered
 * requests. Beyond it the transport answers 503 itself, as the gateway does
 * when its worker budget is exhausted (fpm_http.c:153-155). */
#define FPM_WORKER_PENDING_MAX 256
/* One year, in seconds. Any longer timer is indistinguishable from "never" for
 * a worker process, and this keeps a non-finite or absurd userland timeout out
 * of struct timeval. */
#define FPM_WORKER_TIMEOUT_MAX 31536000.0

/* Watcher kinds accepted by fpmng_worker_event_create(). Mirrors what
 * Revolt's AbstractDriver activates (readable/writable streams and timers);
 * signals are absent on purpose — the FPM master owns SIGQUIT/SIGUSR2 and a
 * userland signal watcher must not compete with it. A driver reports that as
 * an unsupported feature. */
#define FPM_WORKER_EV_READ 1
#define FPM_WORKER_EV_WRITE 2
#define FPM_WORKER_EV_TIMER 3

struct fpm_worker_pending {
	struct evhttp_request *http;	/* NULL after the connection died */
	zend_ulong id;
};

struct fpm_worker_watcher {
	struct event *ev;
	zval callback;
	/* libevent watches a descriptor, userland owns a stream. Without a
	 * reference of our own, closing the stream (or dropping its last
	 * reference) closes the fd under an enabled watcher; the number is then
	 * reused by the next accept()/open() in a long-lived worker and the
	 * watcher starts firing for an unrelated descriptor. IS_UNDEF for
	 * timers. */
	zval stream;
	zend_ulong id;
};

static struct {
	struct fpm_worker_pool_s *wp;
	struct event_base *base;
	struct evhttp *http;
	struct evhttp_bound_socket *listener;
	char root[PATH_MAX];
	char script[PATH_MAX];
	char server_addr[NI_MAXHOST];
	char server_port[NI_MAXSERV];
	int notify_read;
	int notify_write;
	zval notify_stream;
	HashTable pending;		/* id -> struct fpm_worker_pending * */
	HashTable watchers;		/* id -> struct fpm_worker_watcher * */
	zend_ulong next_id;
	zend_ulong ready[FPM_WORKER_PENDING_MAX];	/* FIFO of ids not yet handed to PHP */
	unsigned ready_head;
	unsigned ready_count;
	unsigned answered;
	bool running;
} fw;

static volatile sig_atomic_t fpm_worker_stopping;

/* }}} */

/* Configuration ---------------------------------------------------------- */

const char *const fpm_http_direct_worker_rejects[] = {
	"fiber.", "supervisor.", "cron.", "chroot", "listen.allowed_clients",
	"pm.status_path", "pm.status_listen", "ping.path", "ping.response",
	"access.log", "access.format", "access.suppress_path",
	/* The worker script never "ends a request", so the child stays in one
	 * stage for its whole life and these master-side deadlines would either
	 * never fire or kill a healthy worker. Rejected instead of silently
	 * unenforced. */
	"request_terminate_timeout", "request_slowlog_timeout", "slowlog",
	NULL
};

/* INI value set in the pool, or NULL when only php.ini applies. Same shape and
 * same admin-before-value order as fpm_coop_pool_ini() in fpm_pool_coop.c;
 * duplicated because that file is compiled only with --enable-fpmng-fiber and
 * a stock binary does not contain it. */
static const char *fpm_worker_pool_ini(struct fpm_worker_pool_s *wp, const char *key)
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

int fpm_http_direct_worker_validate(struct fpm_worker_pool_s *wp)
{
	struct fpm_worker_pool_config_s *c = wp->config;
	const char *p = c->set_directives;
	const char *timeout_ini;
	zend_long timeout;
	char root[PATH_MAX], script[PATH_MAX], candidate[PATH_MAX];
	struct stat st;

	if (c->pm != PM_STYLE_STATIC) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor = worker requires pm = static", c->name);
		return -1;
	}
	if (!c->chdir || c->chdir[0] != '/' || !c->http_front_controller || c->http_front_controller[0] != '/' ||
		strstr(c->http_front_controller, "..") || strchr(c->http_front_controller, '\\')) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor = worker requires an absolute chdir and a root-relative "
			"http.front_controller (here: the worker script) without '..' or backslashes", c->name);
		return -1;
	}
	/* Same allow-list as http-direct (fpm_http_direct.c:86-95): a gateway
	 * option must never silently appear to protect a direct worker. */
	while (p && (p = strstr(p, ";http."))) {
		const char *end = strchr(++p, ';');
		size_t len = end ? (size_t) (end - p) : strlen(p);
		if (!((len == sizeof("http.front_controller") - 1 && !strncmp(p, "http.front_controller", len)) ||
			(len == sizeof("http.read_timeout") - 1 && !strncmp(p, "http.read_timeout", len)) ||
			(len == sizeof("http.max_body") - 1 && !strncmp(p, "http.max_body", len)))) {
			zlog(ZLOG_ALERT, "[pool %s] '%.*s' is not supported by pool.type = http-direct with pool.executor = worker",
				c->name, (int) len, p);
			return -1;
		}
	}
	if (c->http_read_timeout <= 0 || c->http_max_body == 0 || c->http_max_body > 32 * 1024 * 1024) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor = worker requires http.read_timeout > 0 and "
			"http.max_body between 1 and 32M", c->name);
		return -1;
	}
	/* The Zend timeout is armed once by php_request_startup(), and here that
	 * single request is the worker's whole lifetime — a non-zero value would
	 * kill the worker mid-service instead of bounding one HTTP request.
	 * fpm_init() runs after php_module_startup() (fpm_main.c), so php.ini is
	 * already loaded and this belongs in the master, not in the child. */
	timeout_ini = fpm_worker_pool_ini(wp, "max_execution_time");
	timeout = timeout_ini ? ZEND_ATOL(timeout_ini)
		: zend_ini_long("max_execution_time", sizeof("max_execution_time") - 1, 0);
	if (timeout != 0) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor = worker: max_execution_time = " ZEND_LONG_FMT " would apply to "
			"the worker script, which runs for the lifetime of the worker, not to one HTTP request; "
			"set php_admin_value[max_execution_time] = 0 in this pool or max_execution_time = 0 in php.ini",
			c->name, timeout);
		return -1;
	}
	/* Catch a bad deployment path in the master, before FPM starts respawning
	 * children that cannot open their worker script. */
	if (!realpath(c->chdir, root) ||
		snprintf(candidate, sizeof(candidate), "%s%s", root, c->http_front_controller) >= (int) sizeof(candidate) ||
		!realpath(candidate, script) || stat(script, &st) < 0 || !S_ISREG(st.st_mode) ||
		strncmp(script, root, strlen(root)) || (strcmp(root, "/") && script[strlen(root)] != '/')) {
		zlog(ZLOG_ALERT, "[pool %s] http-direct worker: the worker script must be a regular file inside chdir",
			c->name);
		return -1;
	}
	return 0;
}

/* Transport --------------------------------------------------------------- */

static void fpm_worker_notify(void)
{
	/* One byte per event. The read end is drained by userland; a full pipe
	 * already means "PHP has not looked yet", so EAGAIN needs no handling. */
	char byte = 1;
	ssize_t ignored;

	/* Before fpm_worker_create_notify_pipe() there is no pipe. fw is a
	 * file-scope struct, so an unguarded write here would go to fd 0 — in an
	 * FPM child that is /dev/null (fpm_stdio.c), so the byte would vanish
	 * silently instead of failing loudly. child_main() sets both ends to -1
	 * before installing the SIGQUIT handler. */
	if (fw.notify_write < 0) {
		return;
	}
	ignored = write(fw.notify_write, &byte, 1);
	(void) ignored;
}

static void fpm_worker_stop_signal(int signo)
{
	(void) signo;
	fpm_worker_stopping = 1;
	fpm_worker_notify();
}

/* Both tables own their values: a worker lives for millions of requests, so a
 * NULL dtor here is a per-request leak, not a shutdown detail. Freeing through
 * the dtor also means every removal path frees exactly once. */
static void fpm_worker_pending_dtor(zval *zv)
{
	pefree(Z_PTR_P(zv), 1);
}

static void fpm_worker_watcher_dtor(zval *zv)
{
	struct fpm_worker_watcher *watcher = Z_PTR_P(zv);

	/* Safe from inside the watcher's own callback, which is how a userland
	 * driver cancels a fired one-shot timer: libevent has already dequeued a
	 * non-persistent event before invoking it, and event_del() on the
	 * currently running event clears event_base->current_event, so nothing
	 * re-arms it afterwards. */
	event_free(watcher->ev);
	zval_ptr_dtor(&watcher->callback);
	zval_ptr_dtor(&watcher->stream);
	pefree(watcher, 1);
}

static void fpm_worker_reap(struct fpm_worker_pending *p)
{
	zend_hash_index_del(&fw.pending, p->id);
}

static void fpm_worker_conn_closed(struct evhttp_connection *connection, void *arg)
{
	struct fpm_worker_pending *p = arg;

	(void) connection;
	/* libevent frees the request with the connection, so only the id survives
	 * here. A later fpmng_worker_respond() on it reports false rather than
	 * writing to a dead connection. */
	p->http = NULL;
	fpm_worker_notify();
}

static void fpm_worker_accept(struct evhttp_request *http, void *arg)
{
	struct fpm_worker_pending *p;

	(void) arg;
	if (fpm_worker_stopping || fw.ready_count >= FPM_WORKER_PENDING_MAX ||
		zend_hash_num_elements(&fw.pending) >= FPM_WORKER_PENDING_MAX) {
		evhttp_add_header(evhttp_request_get_output_headers(http), "Connection", "close");
		evhttp_send_error(http, 503, "Worker unavailable");
		/* Saturation must not be permanent. A pending entry is removed only by
		 * fpmng_worker_respond(), so a handler that returns without answering
		 * burns its slot for the life of the worker; after
		 * FPM_WORKER_PENDING_MAX such leaks every later request would get 503
		 * forever, and nothing would notice — request_terminate_timeout is
		 * rejected by this executor and pm.max_requests only counts answered
		 * requests. Ask for a graceful stop instead: in-flight work drains,
		 * the child exits, the master respawns it. A genuine burst of more
		 * than FPM_WORKER_PENDING_MAX concurrent requests therefore recycles
		 * the worker as well; that is the same drain pm.max_requests performs,
		 * and the pool is already answering 503 at that point. */
		if (!fpm_worker_stopping) {
			zlog(ZLOG_WARNING, "[pool %s] http-direct worker: %d requests accepted but unanswered; "
				"answering 503 and asking the worker script to stop so the master can respawn it",
				fw.wp->config->name, FPM_WORKER_PENDING_MAX);
			fpm_worker_stopping = 1;
			fpm_worker_notify();
		}
		return;
	}
	/* Origin-form only, exactly as the classic direct transport requires
	 * (fpm_http_direct.c:303): the worker script is chosen by configuration,
	 * never by the URI. */
	if (!evhttp_request_get_uri(http) || evhttp_request_get_uri(http)[0] != '/' ||
		!evhttp_request_get_evhttp_uri(http) ||
		evhttp_uri_get_fragment(evhttp_request_get_evhttp_uri(http))) {
		evhttp_send_error(http, 400, "Bad request");
		return;
	}
	p = pemalloc(sizeof(*p), 1);
	p->http = http;
	p->id = fw.next_id++;
	zend_hash_index_add_new_ptr(&fw.pending, p->id, p);
	fw.ready[(fw.ready_head + fw.ready_count) % FPM_WORKER_PENDING_MAX] = p->id;
	fw.ready_count++;
	/* evhttp serves one request per connection at a time, so at most one
	 * pending request per connection can be waiting for a close notice. */
	evhttp_connection_set_closecb(evhttp_request_get_connection(http), fpm_worker_conn_closed, p);
	fpm_worker_notify();
}

static void fpm_worker_watcher_fire(evutil_socket_t fd, short events, void *arg)
{
	struct fpm_worker_watcher *watcher = arg;
	zval callback, retval;

	(void) fd;
	(void) events;
	/* Never call into PHP with an exception already pending: the next call
	 * would run with a poisoned engine state. Leave the remaining watchers to
	 * the iteration after userland has handled it. */
	if (EG(exception)) {
		event_base_loopbreak(fw.base);
		return;
	}
	/* Own the callback for the duration of the call. Freeing a watcher from
	 * inside its own callback is the advertised cancellation idiom (see
	 * fpm_worker_watcher_dtor), and that releases watcher->callback while this
	 * call is still on the stack. A Closure survives because
	 * zend_call_function() addrefs the closure object, but an [$obj, 'method']
	 * callable or an invokable object held by nothing else would be destroyed
	 * mid-method. */
	ZVAL_COPY(&callback, &watcher->callback);
	ZVAL_UNDEF(&retval);
	if (call_user_function(NULL, NULL, &callback, &retval, 0, NULL) == FAILURE || EG(exception)) {
		event_base_loopbreak(fw.base);
	}
	zval_ptr_dtor(&retval);
	zval_ptr_dtor(&callback);
}

/* SAPI ------------------------------------------------------------------- */

static size_t fpm_worker_ub_write(const char *str, size_t len)
{
	size_t written = 0;

	/* One php_request_startup() covers the whole worker, so there is no
	 * per-request output buffer to route this into: worker output is the
	 * worker's, and FPM already collects child stderr into the error log when
	 * catch_workers_output is on. A handler returns its response body. */
	while (written < len) {
		ssize_t n = write(STDERR_FILENO, str + written, len - written);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		written += (size_t) n;
	}
	return written;
}

static int fpm_worker_send_headers(sapi_headers_struct *headers)
{
	(void) headers;
	return SAPI_HEADER_SENT_SUCCESSFULLY;
}

static void fpm_worker_flush(void *context)
{
	(void) context;
}

static char *fpm_worker_getenv(const char *name, size_t len)
{
	(void) len;
	return getenv(name);
}

static size_t fpm_worker_read_post(char *buffer, size_t size)
{
	(void) buffer;
	(void) size;
	return 0;
}

static char *fpm_worker_read_cookies(void)
{
	return NULL;
}

static void fpm_worker_register_variables(zval *array)
{
	php_import_environment_variables(array);
	php_register_variable("SCRIPT_FILENAME", fw.script, array);
	php_register_variable("SCRIPT_NAME", fw.wp->config->http_front_controller, array);
	php_register_variable("DOCUMENT_ROOT", fw.root, array);
	php_register_variable("SERVER_SOFTWARE", "php-fpm-ng/http-direct-worker", array);
	php_register_variable("SERVER_ADDR", fw.server_addr, array);
	php_register_variable("SERVER_PORT", fw.server_port, array);
}

/* PHP-callable surface ---------------------------------------------------- */

static struct fpm_worker_pending *fpm_worker_pending_get(zend_long id)
{
	return id > 0 ? zend_hash_index_find_ptr(&fw.pending, (zend_ulong) id) : NULL;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_notify_stream, 0, 0, IS_RESOURCE, 0)
ZEND_END_ARG_INFO()

/* The read end of the worker's notification pipe. It becomes readable when a
 * request is queued, when a client disconnects, or when the worker is asked to
 * stop. Userland registers ONE readable watcher on it: that watcher both
 * delivers work and keeps a userland event loop from returning from run() for
 * lack of anything to wait on. */
static ZEND_FUNCTION(fpmng_worker_notify_stream)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_COPY(&fw.notify_stream);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_stopping, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

static ZEND_FUNCTION(fpmng_worker_stopping)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_BOOL(fpm_worker_stopping);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_next_request, 0, 0, IS_LONG, 1)
ZEND_END_ARG_INFO()

static ZEND_FUNCTION(fpmng_worker_next_request)
{
	zend_ulong id;

	ZEND_PARSE_PARAMETERS_NONE();
	while (fw.ready_count) {
		id = fw.ready[fw.ready_head];
		fw.ready_head = (fw.ready_head + 1) % FPM_WORKER_PENDING_MAX;
		fw.ready_count--;
		/* Skip anything whose client vanished while it sat in the queue: a
		 * handler must not be started for a connection that cannot be
		 * answered. */
		struct fpm_worker_pending *p = zend_hash_index_find_ptr(&fw.pending, id);
		if (p && p->http) {
			RETURN_LONG((zend_long) id);
		}
		if (p) {
			fpm_worker_reap(p);
		}
	}
	RETURN_NULL();
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_request_env, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* CGI-shaped request metadata, built on demand from the live evhttp request.
 * Same variable set and the same exclusions as the classic direct transport
 * (fpm_http_direct.c:311-346), including dropping "Proxy" (httpoxy). */
static ZEND_FUNCTION(fpmng_worker_request_env)
{
	zend_long id;
	struct fpm_worker_pending *p;
	const struct evhttp_uri *uri;
	struct evkeyvalq *headers;
	struct evkeyval *kv;
	const char *method;
	char *peer = NULL;
	char buf[64];
	ev_uint16_t port = 0;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(id)
	ZEND_PARSE_PARAMETERS_END();

	p = fpm_worker_pending_get(id);
	if (!p || !p->http) {
		RETURN_EMPTY_ARRAY();
	}
	uri = evhttp_request_get_evhttp_uri(p->http);
	headers = evhttp_request_get_input_headers(p->http);
	switch (evhttp_request_get_command(p->http)) {
		case EVHTTP_REQ_GET: method = "GET"; break;
		case EVHTTP_REQ_POST: method = "POST"; break;
		case EVHTTP_REQ_HEAD: method = "HEAD"; break;
		case EVHTTP_REQ_PUT: method = "PUT"; break;
		case EVHTTP_REQ_DELETE: method = "DELETE"; break;
		case EVHTTP_REQ_OPTIONS: method = "OPTIONS"; break;
		case EVHTTP_REQ_PATCH: method = "PATCH"; break;
		default: method = "GET"; break;
	}
	array_init(return_value);
/* Split in two on purpose: gcc -Waddress rejects a NULL test on an array or a
 * string literal ("the address of 'buf' will always evaluate as true"), and
 * only the libevent getters below can actually return NULL. */
#define ENV_STR(key, value) add_assoc_string(return_value, key, (value) ? (value) : "")
#define ENV_FIXED(key, value) add_assoc_string(return_value, key, value)
	ENV_STR("REQUEST_METHOD", method);
	ENV_STR("REQUEST_URI", evhttp_request_get_uri(p->http));
	ENV_STR("QUERY_STRING", evhttp_uri_get_query(uri));
	ENV_STR("PATH_INFO", evhttp_uri_get_path(uri));
	ENV_FIXED("SCRIPT_FILENAME", fw.script);
	ENV_STR("SCRIPT_NAME", fw.wp->config->http_front_controller);
	ENV_FIXED("DOCUMENT_ROOT", fw.root);
	ENV_FIXED("SERVER_SOFTWARE", "php-fpm-ng/http-direct-worker");
	ENV_FIXED("GATEWAY_INTERFACE", "CGI/1.1");
	ENV_FIXED("SERVER_ADDR", fw.server_addr);
	ENV_FIXED("SERVER_PORT", fw.server_port);
	ENV_STR("SERVER_NAME", evhttp_request_get_host(p->http));
	snprintf(buf, sizeof(buf), "HTTP/%d.%d", p->http->major, p->http->minor);
	ENV_FIXED("SERVER_PROTOCOL", buf);
	evhttp_connection_get_peer(evhttp_request_get_connection(p->http), &peer, &port);
	ENV_STR("REMOTE_ADDR", peer);
	snprintf(buf, sizeof(buf), "%u", (unsigned) port);
	ENV_FIXED("REMOTE_PORT", buf);
	snprintf(buf, sizeof(buf), "%zu", evbuffer_get_length(evhttp_request_get_input_buffer(p->http)));
	ENV_FIXED("CONTENT_LENGTH", buf);
	ENV_STR("CONTENT_TYPE", evhttp_find_header(headers, "Content-Type"));
	for (kv = headers->tqh_first; kv; kv = kv->next.tqe_next) {
		char name[FPM_WORKER_HEADERS_MAX + 6];
		size_t i, len = strlen(kv->key);

		if (!strcasecmp(kv->key, "Content-Type") || !strcasecmp(kv->key, "Content-Length") ||
			!strcasecmp(kv->key, "Proxy") || len > FPM_WORKER_HEADERS_MAX) {
			continue;
		}
		memcpy(name, "HTTP_", 5);
		for (i = 0; i < len; i++) {
			unsigned char ch = (unsigned char) kv->key[i];
			name[5 + i] = ch == '-' ? '_' : (char) toupper(ch);
		}
		name[5 + len] = '\0';
		ENV_STR(name, kv->value);
	}
#undef ENV_STR
#undef ENV_FIXED
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_request_body, 0, 1, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* Drains the request body: a second call returns "". http.max_body already
 * bounds it in libevent (evhttp_set_max_body_size). */
static ZEND_FUNCTION(fpmng_worker_request_body)
{
	zend_long id;
	struct fpm_worker_pending *p;
	struct evbuffer *in;
	size_t len;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(id)
	ZEND_PARSE_PARAMETERS_END();

	p = fpm_worker_pending_get(id);
	if (!p || !p->http) {
		RETURN_EMPTY_STRING();
	}
	in = evhttp_request_get_input_buffer(p->http);
	len = evbuffer_get_length(in);
	if (!len) {
		RETURN_EMPTY_STRING();
	}
	zend_string *body = zend_string_alloc(len, 0);
	if (evbuffer_remove(in, ZSTR_VAL(body), len) < 0) {
		zend_string_efree(body);
		RETURN_EMPTY_STRING();
	}
	ZSTR_VAL(body)[len] = '\0';
	RETURN_NEW_STR(body);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_respond, 0, 4, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, status, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, headers, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, body, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* A header name is an RFC 9110 token. libevent 2.1 already rejects CR and LF
 * in both key and value — measured: evhttp_add_header() returns -1 for
 * "X-A\r\nInjected" and for a value containing CRLF — so there is no
 * response-splitting vector here, but it stores a key containing a space or a
 * colon verbatim and emits a malformed header line. Reject those ourselves. */
static bool fpm_worker_header_name_ok(const char *name)
{
	const char *c;

	if (!*name) {
		return false;
	}
	for (c = name; *c; c++) {
		if (!strchr("!#$%&'*+-.^_`|~", *c) && !isalnum((unsigned char) *c)) {
			return false;
		}
	}
	return true;
}

/* Returns false when a header could not be emitted, which the caller turns
 * into a 500 rather than a response missing a header the application asked
 * for — the same contract as the classic transport, which raises r->overflow
 * on a rejected header (fpm_http_direct.c:214-216). */
static bool fpm_worker_add_header(struct evkeyvalq *out, const char *name, zval *value, size_t *total)
{
	zend_string *str;
	bool ok = true;

	if (Z_TYPE_P(value) == IS_ARRAY) {
		zval *item;
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(value), item) {
			if (!fpm_worker_add_header(out, name, item, total)) {
				ok = false;
			}
		} ZEND_HASH_FOREACH_END();
		return ok;
	}
	/* This transport owns framing exactly as the classic one does
	 * (fpm_http_direct.c:207-213): an application-supplied length or
	 * Transfer-Encoding must not desynchronize the next keep-alive request. */
	if (!strcasecmp(name, "Content-Length") || !strcasecmp(name, "Transfer-Encoding") ||
		!strcasecmp(name, "Connection") || !strcasecmp(name, "Keep-Alive") ||
		!strcasecmp(name, "Upgrade") || !strcasecmp(name, "Trailer")) {
		return true;
	}
	if (!fpm_worker_header_name_ok(name)) {
		return false;
	}
	str = zval_try_get_string(value);
	if (!str) {
		return false;
	}
	*total += strlen(name) + ZSTR_LEN(str);
	ok = *total <= FPM_WORKER_HEADERS_MAX && evhttp_add_header(out, name, ZSTR_VAL(str)) == 0;
	zend_string_release(str);
	return ok;
}

/* Answers a request, possibly long after the loop iteration that produced it —
 * that deferred reply is the whole point of the mode. false means the client is
 * gone or the id is unknown; the handler decides whether that is worth
 * logging. */
static ZEND_FUNCTION(fpmng_worker_respond)
{
	zend_long id, status;
	HashTable *headers;
	zend_string *body;
	struct fpm_worker_pending *p;
	struct evbuffer *out;
	zend_string *key;
	zval *value;
	size_t total = 0;
	bool headers_ok = true;

	ZEND_PARSE_PARAMETERS_START(4, 4)
		Z_PARAM_LONG(id)
		Z_PARAM_LONG(status)
		Z_PARAM_ARRAY_HT(headers)
		Z_PARAM_STR(body)
	ZEND_PARSE_PARAMETERS_END();

	/* Final statuses only, exactly the range the classic transport clamps to
	 * (fpm_http_direct.c:454-459). evhttp_send_reply() would happily emit a
	 * 1xx status line as if it were the response and still frame and append
	 * the body, leaving the next keep-alive response behind bytes the client
	 * never read as one. */
	if (status < 200 || status > 599) {
		zend_argument_value_error(2, "must be a final HTTP status between 200 and 599");
		RETURN_THROWS();
	}
	if (ZSTR_LEN(body) > FPM_WORKER_BODY_MAX) {
		zend_argument_value_error(4, "must not exceed %d bytes in this POC", FPM_WORKER_BODY_MAX);
		RETURN_THROWS();
	}
	p = fpm_worker_pending_get(id);
	if (!p) {
		RETURN_FALSE;
	}
	if (!p->http) {
		fpm_worker_reap(p);
		RETURN_FALSE;
	}
	ZEND_HASH_FOREACH_STR_KEY_VAL(headers, key, value) {
		if (!key || !fpm_worker_add_header(evhttp_request_get_output_headers(p->http), ZSTR_VAL(key),
				value, &total)) {
			headers_ok = false;
		}
	} ZEND_HASH_FOREACH_END();
	if (!headers_ok) {
		/* Silently dropping a header the application set is worse than an
		 * error status: it can strip a Content-Security-Policy or a Set-Cookie
		 * and the handler would never learn. evhttp_send_error() clears the
		 * output headers it was about to emit. */
		evhttp_connection_set_closecb(evhttp_request_get_connection(p->http), NULL, NULL);
		evhttp_send_error(p->http, 500, NULL);
		fpm_worker_reap(p);
		RETURN_FALSE;
	}

	out = evbuffer_new();
	if (!out) {
		RETURN_FALSE;
	}
	/* libevent omits framing headers for 204/304 but still appends a supplied
	 * body, which would leave unframed bytes in front of the next keep-alive
	 * response (fpm_http_direct.c:454-459). */
	if (evhttp_request_get_command(p->http) != EVHTTP_REQ_HEAD && status != 204 && status != 205 &&
		status != 304 && ZSTR_LEN(body)) {
		evbuffer_add(out, ZSTR_VAL(body), ZSTR_LEN(body));
	}
	if (fpm_worker_stopping) {
		evhttp_add_header(evhttp_request_get_output_headers(p->http), "Connection", "close");
	}
	/* Drop the close callback before handing the request back: libevent owns
	 * and frees it from here on, and a later close on this connection must not
	 * reach a reaped pending entry. */
	evhttp_connection_set_closecb(evhttp_request_get_connection(p->http), NULL, NULL);
	evhttp_send_reply(p->http, (int) status, NULL, out);
	evbuffer_free(out);
	fpm_worker_reap(p);

	fw.answered++;
	if (fw.wp->config->pm_max_requests && fw.answered >= (unsigned) fw.wp->config->pm_max_requests &&
		!fpm_worker_stopping) {
		/* Recycling is the master's contract with pm.max_requests; the worker
		 * script sees it as an ordinary stop request and drains. */
		fpm_worker_stopping = 1;
		fpm_worker_notify();
	}
	RETURN_TRUE;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_event_create, 0, 3, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, type, IS_LONG, 0)
	ZEND_ARG_INFO(0, stream)
	ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()

static ZEND_FUNCTION(fpmng_worker_event_create)
{
	zend_long type;
	zval *stream, *callback;
	struct fpm_worker_watcher *watcher;
	php_stream *php_stream_handle;
	int fd = -1;	/* php_stream_cast() writes an int for PHP_STREAM_AS_FD_FOR_SELECT */
	short flags;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_LONG(type)
		Z_PARAM_ZVAL(stream)
		Z_PARAM_ZVAL(callback)
	ZEND_PARSE_PARAMETERS_END();

	if (!zend_is_callable(callback, 0, NULL)) {
		zend_argument_type_error(3, "must be a valid callback");
		RETURN_THROWS();
	}
	switch (type) {
		case FPM_WORKER_EV_READ: flags = EV_READ | EV_PERSIST; break;
		case FPM_WORKER_EV_WRITE: flags = EV_WRITE | EV_PERSIST; break;
		/* Timers are one-shot: a repeating userland callback is re-armed by its
		 * own driver, which is how Revolt's ext-event driver behaves too. */
		case FPM_WORKER_EV_TIMER: flags = 0; break;
		default:
			zend_argument_value_error(1, "must be one of FPMNG_WORKER_READ, FPMNG_WORKER_WRITE, FPMNG_WORKER_TIMER");
			RETURN_THROWS();
	}
	if (type != FPM_WORKER_EV_TIMER) {
		if (Z_TYPE_P(stream) != IS_RESOURCE) {
			zend_argument_type_error(2, "must be a stream resource for read and write watchers");
			RETURN_THROWS();
		}
		php_stream_from_zval_no_verify(php_stream_handle, stream);
		if (!php_stream_handle ||
			php_stream_cast(php_stream_handle, PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL,
				(void *) &fd, 1) != SUCCESS || fd < 0) {
			/* The trap the design note names: userland works on PHP streams,
			 * libevent works on descriptors. A stream with buffered userland
			 * data (filters, TLS) can hold bytes the descriptor will never
			 * report as readable. */
			zend_argument_type_error(2, "must be a stream with a usable file descriptor");
			RETURN_THROWS();
		}
	}
	watcher = pemalloc(sizeof(*watcher), 1);
	watcher->id = fw.next_id++;
	ZVAL_COPY(&watcher->callback, callback);
	if (type == FPM_WORKER_EV_TIMER) {
		ZVAL_UNDEF(&watcher->stream);
	} else {
		ZVAL_COPY(&watcher->stream, stream);
	}
	watcher->ev = event_new(fw.base, type == FPM_WORKER_EV_TIMER ? -1 : fd, flags,
		fpm_worker_watcher_fire, watcher);
	if (!watcher->ev) {
		/* Not in the table yet, so the dtor never sees it. */
		zval_ptr_dtor(&watcher->callback);
		zval_ptr_dtor(&watcher->stream);
		pefree(watcher, 1);
		zend_throw_error(NULL, "fpmng_worker_event_create(): failed to create a libevent event");
		RETURN_THROWS();
	}
	zend_hash_index_add_new_ptr(&fw.watchers, watcher->id, watcher);
	RETURN_LONG((zend_long) watcher->id);
}

static struct fpm_worker_watcher *fpm_worker_watcher_get(zend_long id)
{
	return id > 0 ? zend_hash_index_find_ptr(&fw.watchers, (zend_ulong) id) : NULL;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_event_enable, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout, IS_DOUBLE, 1, "null")
ZEND_END_ARG_INFO()

static ZEND_FUNCTION(fpmng_worker_event_enable)
{
	zend_long id;
	double timeout = 0;
	bool timeout_is_null = true;
	struct fpm_worker_watcher *watcher;
	struct timeval tv;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_LONG(id)
		Z_PARAM_OPTIONAL
		Z_PARAM_DOUBLE_OR_NULL(timeout, timeout_is_null)
	ZEND_PARSE_PARAMETERS_END();

	watcher = fpm_worker_watcher_get(id);
	if (!watcher) {
		RETURN_FALSE;
	}
	if (timeout_is_null) {
		RETURN_BOOL(event_add(watcher->ev, NULL) == 0);
	}
	/* Written as "not greater than zero" so NAN lands here too: casting NAN or
	 * INF to time_t is undefined behaviour, and fpmng_worker_event_enable($id,
	 * INF) is one typo away in a userland driver. */
	if (!(timeout > 0)) {
		timeout = 0;
	} else if (timeout > FPM_WORKER_TIMEOUT_MAX) {
		timeout = FPM_WORKER_TIMEOUT_MAX;
	}
	tv.tv_sec = (time_t) timeout;
	tv.tv_usec = (suseconds_t) ((timeout - (double) tv.tv_sec) * 1000000);
	RETURN_BOOL(event_add(watcher->ev, &tv) == 0);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_event_disable, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
ZEND_END_ARG_INFO()

static ZEND_FUNCTION(fpmng_worker_event_disable)
{
	zend_long id;
	struct fpm_worker_watcher *watcher;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(id)
	ZEND_PARSE_PARAMETERS_END();

	watcher = fpm_worker_watcher_get(id);
	RETURN_BOOL(watcher && event_del(watcher->ev) == 0);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_event_free, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
ZEND_END_ARG_INFO()

static ZEND_FUNCTION(fpmng_worker_event_free)
{
	zend_long id;
	struct fpm_worker_watcher *watcher;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(id)
	ZEND_PARSE_PARAMETERS_END();

	watcher = fpm_worker_watcher_get(id);
	if (!watcher) {
		RETURN_FALSE;
	}
	/* fpm_worker_watcher_dtor() does the event_free() and the callback release. */
	zend_hash_index_del(&fw.watchers, watcher->id);
	RETURN_TRUE;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_loop, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, blocking, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

/* One iteration of the worker's event loop. This is the only place the base is
 * ever driven, which is what keeps libevent's reentrancy guard satisfied: a
 * watcher callback must never call this again. */
static ZEND_FUNCTION(fpmng_worker_loop)
{
	bool blocking;
	int result;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_BOOL(blocking)
	ZEND_PARSE_PARAMETERS_END();

	if (fw.running) {
		zend_throw_error(NULL, "fpmng_worker_loop(): the event loop is already running; "
			"libevent allows only one event_base_loop() per base at a time");
		RETURN_THROWS();
	}
	fw.running = true;
	result = event_base_loop(fw.base, EVLOOP_ONCE | (blocking ? 0 : EVLOOP_NONBLOCK));
	fw.running = false;
	if (EG(exception)) {
		RETURN_THROWS();
	}
	RETURN_BOOL(result >= 0);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_loop_break, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

static ZEND_FUNCTION(fpmng_worker_loop_break)
{
	ZEND_PARSE_PARAMETERS_NONE();
	event_base_loopbreak(fw.base);
}

static const zend_function_entry fpm_worker_functions[] = {
	ZEND_FE(fpmng_worker_notify_stream, arginfo_fpmng_worker_notify_stream)
	ZEND_FE(fpmng_worker_stopping, arginfo_fpmng_worker_stopping)
	ZEND_FE(fpmng_worker_next_request, arginfo_fpmng_worker_next_request)
	ZEND_FE(fpmng_worker_request_env, arginfo_fpmng_worker_request_env)
	ZEND_FE(fpmng_worker_request_body, arginfo_fpmng_worker_request_body)
	ZEND_FE(fpmng_worker_respond, arginfo_fpmng_worker_respond)
	ZEND_FE(fpmng_worker_event_create, arginfo_fpmng_worker_event_create)
	ZEND_FE(fpmng_worker_event_enable, arginfo_fpmng_worker_event_enable)
	ZEND_FE(fpmng_worker_event_disable, arginfo_fpmng_worker_event_disable)
	ZEND_FE(fpmng_worker_event_free, arginfo_fpmng_worker_event_free)
	ZEND_FE(fpmng_worker_loop, arginfo_fpmng_worker_loop)
	ZEND_FE(fpmng_worker_loop_break, arginfo_fpmng_worker_loop_break)
	ZEND_FE_END
};

/* task 076: zend_register_functions() unconditionally does
 * `internal_function->module = EG(current_module)` (Zend/zend_API.c:3060),
 * independent of the `type` argument we pass it. EG(current_module) is only
 * ever non-NULL while a module's own MINIT is running; by the time this SAPI
 * calls it -- in the forked worker child, long after every module's startup
 * -- it is NULL. That NULL then reaches opcache: pass1 constant-folds
 * function_exists()/is_callable() on a literal argument and dereferences
 * func->module->type with no NULL check
 * (Zend/Optimizer/zend_optimizer.c:114, PHP 8.5), so a worker script
 * containing e.g. `function_exists('fpmng_worker_loop')` segfaulted at
 * opcache compile time. Confirmed from a core dump on the test box:
 * #0 zend_optimizer_eval_special_func_call (zend_optimizer.c:114)
 * #1 zend_optimizer_pass1 (Zend/Optimizer/pass1.c:254)
 * ... cache_script_in_shared_memory -> php_execute_script ->
 * fpm_http_direct_worker_child_main, faulting instruction
 * `cmpb $0x1,0x8c(%rax)` with rax = 0 (func->module; offset 0x8c is
 * zend_module_entry.type, compared against 1 = MODULE_PERSISTENT).
 *
 * opcache reads only ->type (and, on Windows, ->handle) from this struct
 * (zend_optimizer.c:106-109):
 *
 *     func->type == ZEND_INTERNAL_FUNCTION && func->module->type == MODULE_PERSISTENT
 *
 * -- and only *folds* function_exists()/is_callable() when that whole
 * condition is true. That is deliberately NOT what we want here: opcache's
 * SHM (and op_array cache) is shared across every pool and every executor in
 * the process tree, keyed on script path, not on which pool compiled it
 * first. If this anchor module claimed MODULE_PERSISTENT, a worker child
 * that happens to compile a shared file first (a common front controller,
 * or examples/http-direct-worker/FpmngDriver.php's own
 * `function_exists('fpmng_worker_loop')` capability check) would bake
 * `true` into that cache entry, and a later classic/fiber/fastcgi child
 * hitting the same cached entry would take the worker branch and crash on
 * an undefined `fpmng_worker_*` call -- nondeterministic across restarts,
 * and durable across a master restart under opcache.file_cache.
 *
 * So this module_entry claims MODULE_TEMPORARY instead: `func->module` is
 * still a valid, non-NULL pointer (fixing the crash), but
 * `func->module->type != MODULE_PERSISTENT` makes the fold condition above
 * false, so pass1 always returns FAILURE and function_exists()/is_callable()
 * fall through to their normal runtime evaluation, per child, every time --
 * which is what's actually correct for a function set that is registered
 * conditionally per fork. It exists purely to be a non-NULL anchor -- never
 * registered in module_registry, no MINIT/MSHUTDOWN, no globals. Registering
 * it for real (zend_register_internal_module) was rejected: that runs at
 * every module's startup with EG(current_module) already pointing at it,
 * which is a much larger surface (module_registry entry, module_number,
 * phpinfo listing) for a struct whose only job is to survive a pointer
 * dereference without being foldable. */
static zend_module_entry fpm_worker_module_entry = {
	.size = sizeof(zend_module_entry),
	.zend_api = ZEND_MODULE_API_NO,
	.zend_debug = ZEND_DEBUG,
	.zts = USING_ZTS,
	.name = "fpmng_worker_builtins",
	.type = MODULE_TEMPORARY,
	.build_id = ZEND_MODULE_BUILD_ID,
};

static zend_result fpm_worker_register_functions(HashTable *function_table)
{
	zend_module_entry *saved_module = EG(current_module);
	zend_result result;

	EG(current_module) = &fpm_worker_module_entry;
	result = zend_register_functions(NULL, fpm_worker_functions, function_table, MODULE_PERSISTENT);
	EG(current_module) = saved_module;
	return result;
}

static void fpm_worker_install_sapi(void)
{
	sapi_module.pre_request_init = NULL;
	sapi_module.deactivate = NULL;
	sapi_module.ub_write = fpm_worker_ub_write;
	sapi_module.flush = fpm_worker_flush;
	sapi_module.getenv = fpm_worker_getenv;
	sapi_module.read_post = fpm_worker_read_post;
	sapi_module.read_cookies = fpm_worker_read_cookies;
	sapi_module.register_server_variables = fpm_worker_register_variables;
	sapi_module.send_headers = fpm_worker_send_headers;
	/* All three cast SG(server_context) to fcgi_request and would crash in a
	 * loop that has no FastCGI request at all; none of them has a meaning when
	 * one PHP request spans many HTTP requests. */
	zend_disable_functions("fastcgi_finish_request,getallheaders,apache_request_headers");
}

/* Child ------------------------------------------------------------------ */

/* Split from the stream wrapping below on purpose. The raw descriptors must
 * exist before the SIGQUIT handler is installed, because that handler writes
 * to fw.notify_write (child_main() creates the pipe before that sigaction()
 * call for exactly this reason); the PHP stream must NOT exist yet, because
 * php_stream_to_zval() registers a resource in EG(regular_list), and that
 * table is only initialised by init_executor() during php_request_startup().
 * Doing both here segfaulted every child at startup, measured on the test box:
 * "child ... exited on signal 11 (SIGSEGV) after 0.14 seconds from start",
 * in a respawn loop, before the listener ever answered. */
static int fpm_worker_create_notify_pipe(void)
{
	int fds[2];

	if (pipe(fds) < 0) {
		return -1;
	}
	fw.notify_read = fds[0];
	fw.notify_write = fds[1];
	/* Both ends non-blocking: the writer is a signal handler and a libevent
	 * callback, neither of which may block, and the reader is drained by
	 * userland from inside a readable callback. */
	if (fcntl(fw.notify_read, F_SETFL, O_NONBLOCK) < 0 || fcntl(fw.notify_write, F_SETFL, O_NONBLOCK) < 0) {
		return -1;
	}
	return 0;
}

/* Must run after php_request_startup(). */
static int fpm_worker_wrap_notify_stream(void)
{
	php_stream *stream = php_stream_fopen_from_fd(fw.notify_read, "r", NULL);

	if (!stream) {
		return -1;
	}
	stream->flags |= PHP_STREAM_FLAG_NO_CLOSE;
	php_stream_to_zval(stream, &fw.notify_stream);
	/* Held for the worker's lifetime: userland receives copies, so a closed or
	 * garbage-collected copy must not take the pipe with it. */
	Z_ADDREF(fw.notify_stream);
	return 0;
}

void fpm_http_direct_worker_child_main(struct fpm_worker_pool_s *wp)
{
	struct timeval timeout = {wp->config->http_read_timeout / 1000, (wp->config->http_read_timeout % 1000) * 1000};
	struct sigaction action = {0}, term_before;
	struct sockaddr_storage address;
	socklen_t address_len = sizeof(address);
	zend_file_handle file;
	struct stat st;
	char candidate[PATH_MAX];

	fw.wp = wp;
	fw.next_id = 1;
	if (!getcwd(fw.root, sizeof(fw.root)) ||
		snprintf(candidate, sizeof(candidate), "%s%s", fw.root, wp->config->http_front_controller) >= (int) sizeof(candidate) ||
		!realpath(candidate, fw.script) || stat(fw.script, &st) < 0 || !S_ISREG(st.st_mode) ||
		strncmp(fw.script, fw.root, strlen(fw.root)) ||
		(strcmp(fw.root, "/") && fw.script[strlen(fw.root)] != '/')) {
		zlog(ZLOG_ERROR, "[pool %s] http-direct worker: the worker script must be a regular file inside chdir",
			wp->config->name);
		exit(FPM_EXIT_CONFIG);
	}
	if (getsockname(wp->listening_socket, (struct sockaddr *) &address, &address_len) == 0) {
		getnameinfo((struct sockaddr *) &address, address_len, fw.server_addr, sizeof(fw.server_addr),
			fw.server_port, sizeof(fw.server_port), NI_NUMERICHOST | NI_NUMERICSERV);
	}
	fw.base = event_base_new();
	fw.http = fw.base ? evhttp_new(fw.base) : NULL;
	if (!fw.http) {
		exit(FPM_EXIT_SOFTWARE);
	}
	evhttp_set_max_headers_size(fw.http, FPM_WORKER_HEADERS_MAX);
	evhttp_set_max_body_size(fw.http, wp->config->http_max_body);
	evhttp_set_timeout_tv(fw.http, &timeout);
	evhttp_set_allowed_methods(fw.http, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD |
		EVHTTP_REQ_PUT | EVHTTP_REQ_DELETE | EVHTTP_REQ_OPTIONS | EVHTTP_REQ_PATCH);
	evhttp_set_gencb(fw.http, fpm_worker_accept, NULL);
	fw.listener = evhttp_accept_socket_with_handle(fw.http, wp->listening_socket);
	if (!fw.listener) {
		exit(FPM_EXIT_SOFTWARE);
	}
	zend_hash_init(&fw.pending, 16, NULL, fpm_worker_pending_dtor, 1);
	zend_hash_init(&fw.watchers, 16, NULL, fpm_worker_watcher_dtor, 1);
	/* Strictly before the sigaction() below: the handler writes to
	 * fw.notify_write, and until the pipe exists that field must be a
	 * descriptor fpm_worker_notify() refuses rather than fd 0. */
	fw.notify_read = fw.notify_write = -1;
	if (fpm_worker_create_notify_pipe() < 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] http-direct worker: failed to create the notification pipe",
			wp->config->name);
		exit(FPM_EXIT_SOFTWARE);
	}
	action.sa_handler = fpm_worker_stop_signal;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGQUIT, &action, NULL) < 0) {
		exit(FPM_EXIT_SOFTWARE);
	}
	fpm_worker_install_sapi();
	/* The child stays in the ACCEPTING stage for its whole life: it never ends
	 * a request in the scoreboard sense, so the master's per-request deadlines
	 * would have nothing to measure — which is why this type rejects them
	 * (fpm_http_direct_worker_rejects). Per-request accounting for a worker
	 * serving many connections at once is task 066 territory. */
	fpm_request_accepting(false);

	memset(&SG(request_info), 0, sizeof(SG(request_info)));
	SG(server_context) = &fw;
	SG(request_info).path_translated = estrdup(fw.script);
	SG(request_info).no_headers = 1;
	/* Zend resets SIGTERM during request startup/shutdown (fpm_pool_script.c);
	 * preserve FPM's immediate termination, SIGQUIT stays our graceful flag. */
	sigaction(SIGTERM, NULL, &term_before);
	if (php_request_startup() == FAILURE) {
		zlog(ZLOG_ERROR, "[pool %s] http-direct worker: PHP request startup failed", wp->config->name);
		exit(FPM_EXIT_SOFTWARE);
	}
	sigaction(SIGTERM, &term_before, NULL);
	if (fpm_worker_register_functions(CG(function_table)) == FAILURE) {
		zlog(ZLOG_ERROR, "[pool %s] http-direct worker: failed to register the fpmng_worker_* functions",
			wp->config->name);
		exit(FPM_EXIT_SOFTWARE);
	}
	if (fpm_worker_wrap_notify_stream() < 0) {
		zlog(ZLOG_ERROR, "[pool %s] http-direct worker: failed to expose the notification pipe as a stream",
			wp->config->name);
		exit(FPM_EXIT_SOFTWARE);
	}
	REGISTER_MAIN_LONG_CONSTANT("FPMNG_WORKER_READ", FPM_WORKER_EV_READ, CONST_PERSISTENT);
	REGISTER_MAIN_LONG_CONSTANT("FPMNG_WORKER_WRITE", FPM_WORKER_EV_WRITE, CONST_PERSISTENT);
	REGISTER_MAIN_LONG_CONSTANT("FPMNG_WORKER_TIMER", FPM_WORKER_EV_TIMER, CONST_PERSISTENT);

	EG(exit_status) = 0;
	zend_first_try {
		zend_stream_init_filename(&file, fw.script);
		file.primary_script = true;
		if (zend_stream_open(&file) == FAILURE) {
			zlog(ZLOG_ERROR, "[pool %s] http-direct worker: cannot open the worker script %s",
				wp->config->name, fw.script);
		} else {
			php_execute_script(&file);
		}
		if (!file.in_list) {
			zend_destroy_file_handle(&file);
		}
	} zend_end_try();
	/* Returning here means the worker script stopped serving. That is expected
	 * on SIGQUIT and on pm.max_requests; otherwise the master will respawn the
	 * child and it will stop again, so name the cause once per exit. */
	if (!fpm_worker_stopping) {
		zlog(ZLOG_WARNING, "[pool %s] http-direct worker: the worker script returned without being asked to "
			"stop (exit status %d); the master will respawn this child",
			wp->config->name, EG(exit_status));
	}
	/* Before php_request_shutdown(), not after: a watcher holds a zval
	 * callback allocated by this request, so releasing it once the request
	 * arena is gone would be a use-after-free. Freeing the events here also
	 * guarantees none outlives event_base_free() below. */
	/* Strictly before zend_hash_destroy(&fw.pending). evhttp_free() closes the
	 * still-open server connections and *does* fire their close callbacks;
	 * measured against libevent 2.1: "before evhttp_free, got_closecb=0" /
	 * "CLOSECB fired" / "after evhttp_free, got_closecb=1". Each of those
	 * callbacks writes p->http = NULL through a struct fpm_worker_pending, so
	 * destroying the table first turns every abandoned request into a heap
	 * write into freed memory. Reachable whenever the worker script stops with
	 * requests still in flight — an escaping exception, exit(), or a handler
	 * that never answered. */
	evhttp_free(fw.http);
	zend_hash_destroy(&fw.watchers);
	zend_hash_destroy(&fw.pending);
	php_request_shutdown(NULL);
	sigaction(SIGTERM, &term_before, NULL);
	SG(server_context) = NULL;
	fpm_stdio_flush_child();
	event_base_free(fw.base);
	exit(FPM_EXIT_OK);
}
