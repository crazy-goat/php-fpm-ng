/* fpm-ng: worker-mode HTTP-direct.
 *
 * Started as an experimental POC (task 073); v0.7.0 hardened it toward
 * production use (worker metrics, worker.max_memory/worker.max_lifetime,
 * fpm_connection_info()/fpm_send_early_hints() parity with the classic
 * executor, TLS, a full .phpt suite) and it now carries FPM_TIER_BETA
 * (fpm_pool_type.c) rather than POC status. Some rough edges remain, listed
 * below as "current limits" rather than POC ones.
 *
 * pool.type = http-direct (fpm_http_direct.c) runs one script per request from
 * inside an evhttp callback: evhttp_set_gencb (fpm_http_direct.c:2451) fires
 * under event_base_dispatch (:2546) and php_execute_script runs there (:2343).
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
 * Current limits, all documented rather than worked around: no per-request
 * isolation (one php_request_startup per worker), so `echo` belongs to the
 * worker (it goes to stderr) and a handler returns its body instead; no
 * per-request scoreboard accounting (fpm_request_accepting(false) once at
 * :2580). TLS is NOT a limit here: this executor terminates it like the
 * classic one since issue #55, see the fpm_http_direct_tls_child_attach()
 * call at :2474. Streaming is no longer one either: fpmng_worker_respond()
 * still takes one complete body, but fpmng_worker_respond_start()/_chunk()/
 * _end() (issue #332) push bytes as the handler produces them, with
 * worker.send_buffer_limit for backpressure.
 */
#include "fpm_config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netdb.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/keyvalq_struct.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
/* issue #338: evconnlistener_enable()/_disable() for the accept ceiling -- the
 * same libevent API classic's accept gate uses. */
#include <event2/listener.h>

#ifdef HAVE_FPM_HTTP_TLS
/* fpm_connection_info()'s TLS fields (issue #335): the same SSL and X509
 * access fpm_http_direct.c's classic implementation uses, reached the same
 * way -- bufferevent_openssl_get_ssl() on the connection's bufferevent. */
#include <event2/bufferevent_ssl.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#endif

#include "php.h"
#include "php_main.h"
#include "php_ini.h"
#include "php_variables.h"
#include "php_streams.h"
#include "zend_smart_str.h"
/* issue #343: Sec-WebSocket-Accept is base64(SHA1(key || GUID)) -- both
 * primitives are PHPAPI in ext/standard, which is always linked. */
#include "ext/standard/base64.h"
#include "ext/standard/sha1.h"
#include "zend_API.h"
#include "zend_exceptions.h"
#include "zend_ini.h"
#include "SAPI.h"
#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_http_direct.h"
#include "fpm_http_direct_worker.h"
#include "fpm_http_direct_worker_metrics.h"
#include "fpm_http_direct_ops.h"
#include "fpm_http_direct_request.h"
#include "fpm_http_direct_user_ini.h"
#include "fpm_http_direct_tls.h"
#include "fpm_http_direct_conn.h"
#include "fpm_http_acl.h"
#include "fpm_std_streams.h"
#include "fpm_php.h"
#include "fpm_request.h"
#include "fpm_scoreboard.h"
#include "fpm_stdio.h"
#include "zlog.h"

#define FPM_WORKER_BODY_MAX (8 * 1024 * 1024)
/* FPM_WORKER_PENDING_MAX (the compile-time default for worker.max_pending) is
 * declared in fpm_http_direct_worker.h, not here: fpm_conf.c needs it too, to
 * seed the directive's default before the pool's config is parsed. */
/* One year, in seconds. Any longer timer is indistinguishable from "never" for
 * a worker process, and this keeps a non-finite or absurd userland timeout out
 * of struct timeval. */
#define FPM_WORKER_TIMEOUT_MAX 31536000.0
/* How long child_main() keeps driving the base to get the final 503s of
 * fpm_worker_finish_output() onto the wire. Bounded because a client that
 * stopped reading must not keep the child alive: the master is already waiting
 * on its own stop timeout at this point, and losing the reply is no worse than
 * the silent close this whole path exists to prevent. */
#define FPM_WORKER_FLUSH_BUDGET 1
/* worker.request_timeout's sweep runs more often than the timeout itself so
 * that an expired request is caught within one sweep interval of its
 * deadline, not one whole timeout period late. Floored so a short timeout
 * (the 200 ms sapi/fpmng/tests case, for one) still sweeps at a sane rate
 * instead of every 50 ms / DIVISOR = 12.5, rounded down to single-digit ms. */
#define FPM_WORKER_REQUEST_TIMEOUT_SWEEP_DIVISOR 4
#define FPM_WORKER_REQUEST_TIMEOUT_SWEEP_FLOOR_MS 25

/* worker.max_memory/worker.max_lifetime (issue #334): how often fw.health_sweep
 * checks the child's own peak RSS and elapsed lifetime. Unlike
 * fw.request_timeout_sweep, this timer is armed whenever EITHER directive is
 * non-zero -- worker.request_timeout's sweep interval is derived from that
 * one directive's own value and cannot be relied on to run at all (it is
 * simply absent when worker.request_timeout = 0, the default). One second is
 * frequent enough that a worker recycles within a second of crossing either
 * limit without the getrusage() call this costs every tick showing up as
 * measurable overhead. */
#define FPM_WORKER_HEALTH_SWEEP_INTERVAL_SEC 1

/* Watcher kinds accepted by fpmng_worker_event_create(). Mirrors what
 * Revolt's AbstractDriver activates (readable/writable streams and timers);
 * signals are absent on purpose — the FPM master owns SIGQUIT/SIGUSR2 and a
 * userland signal watcher must not compete with it. A driver reports that as
 * an unsupported feature. */
/* How many consecutive iterations a read watcher may be invoked for a userland
 * buffer that never shrinks before we say so in the log. The re-cast loop in
 * fpm_worker_activate_buffered() makes read watchers level-triggered, which
 * trades a silent hang for a spin; 100% CPU with nothing in the log would be
 * the worse of the two, so the spin is named. 100 iterations is short enough to
 * appear immediately and long enough that no legitimate consumer reaches it:
 * progress is measured as the buffer being smaller when we look at it than it
 * was when we last handed it over, so a consumer that reads anything at all
 * resets the count, and one that reads until the read comes up short empties
 * the buffer entirely. */
#define FPM_WORKER_SPIN_LIMIT 100

#define FPM_WORKER_EV_READ 1
#define FPM_WORKER_EV_WRITE 2
#define FPM_WORKER_EV_TIMER 3

struct fpm_worker_pending {
	struct evhttp_request *http;	/* NULL after the connection died */
	zend_ulong id;
	/* issue #331: when fpm_worker_accept() queued this entry, for
	 * worker.request_timeout's sweep (fpm_worker_sweep_expired()) to compare
	 * against "now". gettimeofday(), the same call fpm_http_direct_conn.c:421
	 * already makes to stamp a connection's accept time -- this file has no
	 * cached libevent clock of its own to reuse instead. */
	struct timeval accepted_at;
	/* issue #332: true from fpmng_worker_respond_start() until
	 * fpmng_worker_respond_end() reaps this entry (or the client's closecb
	 * ends the stream server-side -- fpm_worker_conn_closed()). Exempts it
	 * from worker.request_timeout's sweep
	 * (fpm_worker_sweep_expired()) -- that directive means "never got its
	 * first byte of response", not "streaming took a while", and a stream can
	 * legitimately run for as long as the client keeps the connection open.
	 * Also what makes fpmng_worker_respond() on the same id, or a second
	 * fpmng_worker_respond_start(), fail cleanly instead of sending two status
	 * lines for one request. fpm_send_early_hints() (issue #335) reuses this
	 * same flag as its "final response already started" guard: a buffered
	 * fpmng_worker_respond() needs no flag of its own for that check because
	 * it reaps the entry outright (fpm_worker_reap()), so a reaped id already
	 * answers NULL from fpm_worker_pending_get() -- streaming is the only case
	 * where the entry is still there but a status line may already be on the
	 * wire. */
	bool streaming;
	/* issue #335: caches fpmng_worker_request_body()'s drained result so a
	 * second call for the same id sees the body it already read instead of an
	 * empty evbuffer. NULL until the first call; released in
	 * fpm_worker_pending_dtor(). */
	zend_string *body_cache;
};

/* issue #343: the WebSocket hijack context, defined with
 * fpmng_worker_upgrade() below. */
struct fpm_ws_ctx;

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
	/* issue #343: non-NULL when the watched stream is an upgraded WebSocket
	 * connection (fpmng_worker_upgrade()). The hijacked connection's data
	 * lands in its bufferevent's input evbuffer -- the descriptor itself goes
	 * quiet once libevent has drained it -- so the fd watcher on its own can
	 * never fire for buffered bytes; the bufferevent callbacks fire the
	 * watcher through this back-pointer instead. */
	struct fpm_ws_ctx *ws;
	/* Busy-spin detection for fpm_worker_activate_buffered(): how many
	 * iterations in a row this watcher was activated for a buffer that did not
	 * shrink, the size it was left holding last time, and whether the warning
	 * already went out (once per spin, not once per iteration). Reset when the
	 * watcher is disabled, since a disabled watcher stops being sampled and its
	 * baseline goes stale. */
	unsigned spin;
	size_t buffered;
	bool spin_warned;
	zend_ulong id;
	/* issue #339: FPM_WORKER_EV_READ/WRITE/TIMER, set once at
	 * fpmng_worker_event_create() and never changed -- the
	 * fpmng_pool_worker_watchers{type=...} gauge's only use for it. */
	short type;
};

/* issue #343: defined in the fpmng_worker_upgrade() block below; the dtor
 * above calls it, which is why the declaration exists. */
static void fpm_ws_unregister_watcher(struct fpm_worker_watcher *watcher);
/* issue #343: fpmng_worker_upgrade() reuses these; both are defined further
 * down, after the pending table's own helpers. */
static struct fpm_worker_pending *fpm_worker_pending_get(zend_long id);
static void fpm_worker_count_scoreboard_request(void);

static struct {
	struct fpm_worker_pool_s *wp;
	struct event_base *base;
	struct evhttp *http;
	struct evhttp_bound_socket *listener;
	/* issue #61: the first-request deadline. The connection limits are not
	 * here -- this executor rejects them, see fpm_http_direct_worker_rejects. */
	struct fpm_http_direct_conns *conns;
	char root[PATH_MAX];
	char script[PATH_MAX];
	char server_addr[NI_MAXHOST];
	char server_port[NI_MAXSERV];
	/* listen.allowed_clients (issue #59), parsed once in child_main(). NULL
	 * means the directive is unset and every peer is allowed. */
	struct fpm_http_acl_s *acl;
	int notify_read;
	int notify_write;
	zval notify_stream;
	HashTable pending;		/* id -> struct fpm_worker_pending * */
	HashTable watchers;		/* id -> struct fpm_worker_watcher * */
	zend_ulong next_id;
	/* issue #331: FIFO of ids not yet handed to PHP, ring-buffered over
	 * ready_max slots. Sized once in child_main() from
	 * wp->config->worker_max_pending (pemalloc'd, since the rest of this
	 * struct's owned allocations are persistent too) and freed on every exit
	 * path child_main() has -- see the pefree() next to evhttp_free() there. */
	zend_ulong *ready;
	unsigned ready_max;
	unsigned ready_head;
	unsigned ready_count;
	/* issue #342: FIFO of ids whose client hung up before the request was
	 * answered -- streamed or not -- drained by fpmng_worker_closed_requests().
	 * Ring-buffered over ready_max slots like fw.ready, and for the same bound:
	 * an id reaches this ring only through fpm_worker_conn_closed(), which an
	 * accepted request can hit at most once, and the pending table never
	 * exceeds ready_max (fpm_worker_accept() refuses new work at that bar).
	 * A userland driver that never drains sees oldest ids dropped, never
	 * garbage: the ring overwrites, it does not corrupt. */
	zend_ulong *closed;
	unsigned closed_max;
	unsigned closed_head;
	unsigned closed_count;
	unsigned answered;
	/* issue #338: worker.accept_threshold, copied once in child_main(). 0 means
	 * the ceiling is off and accept_taken/accept_closed stay untouched. */
	unsigned accept_threshold;
	/* Connections accepted in the current accept window: incremented by
	 * fpm_worker_accept_hook(), reset when the gate reopens. */
	unsigned accept_taken;
	/* True while this file has the listener disabled, so that the re-enable only
	 * ever undoes a disable this file did -- a retiring worker's
	 * evhttp_del_accept_socket() must not be reversed. */
	bool accept_closed;
	/* The cooldown the gate owes its siblings: a one-shot timer armed whenever
	 * the gate closes (NULL when the ceiling is off), and the flag that is true
	 * from the arming until it fires. */
	struct event *accept_cooldown;
	bool accept_cooling;
	/* Replies handed to libevent whose bytes are not on the socket yet. Only
	 * the shutdown path reads it; see fpm_worker_finish_output(). */
	unsigned unflushed;
	bool running;
	/* True for the whole of fpm_worker_finish_output()'s teardown walk (issue
	 * #444): that walk iterates fw.pending un-snapshotted, so a close callback
	 * firing during its bounded flush wait must not reap behind its back --
	 * zend_hash_destroy() owns whatever is left when it returns. */
	bool finishing;
	/* issue #331: worker.request_timeout. NULL when the directive is 0 (off) --
	 * "off means off", not a sweep that runs and never expires anything -- and
	 * otherwise a persistent libevent timer re-armed by
	 * fpm_worker_sweep_expired() itself, one sweep over fw.pending rather than
	 * one libevent timer per pending request: up to worker.max_pending (default
	 * 256) requests can be held at once, and re-arming 256 individual one-shot
	 * timers on every accept/respond is 256 event_add()s of bookkeeping this
	 * file does not otherwise need, against one periodic timer whose callback
	 * is O(pending) either way (fpm_worker_finish_output() already walks the
	 * whole table). This file already has precedent for a periodic deadline
	 * timer over a per-entry one -- fpm_worker_flush_deadline() bounds
	 * fpm_worker_finish_output()'s whole wait with a single timer rather than
	 * one per unflushed reply. */
	struct event *request_timeout_sweep;
	/* issue #334: worker.max_memory/worker.max_lifetime's periodic check,
	 * armed in child_main() whenever either directive is non-zero -- see the
	 * field comment on FPM_WORKER_HEALTH_SWEEP_INTERVAL_SEC for why this is a
	 * timer of its own rather than reusing request_timeout_sweep above. NULL
	 * when both directives are 0 (off), the same "off means off" contract
	 * request_timeout_sweep already has. */
	struct event *health_sweep;
	/* issue #334: when this child's php_request_startup() began, for
	 * worker.max_lifetime to compare "now" against. time(NULL) has
	 * second-granularity, which matches worker.max_lifetime's own unit. */
	time_t start_time;
	/* issue #333: this child's shared-memory handle for the pending/watcher
	 * gauges the operator endpoint reads (fpm_http_direct_worker_metrics.c).
	 * NULL is a valid value throughout -- fpm_worker_metrics_publish() is a
	 * no-op on it -- so a pool whose init_main() failed to allocate the
	 * segment still runs, just without these two gauges. */
	struct fpm_worker_metrics *metrics;
	/* issue #339: this child's slot in fpm_http_direct_ops's shared table,
	 * the same one the classic executor uses for pm.status_path's ?full rows
	 * -- see fpm_pool_type_http_direct_worker_init()'s comment for why this
	 * executor now allocates it too. NULL is a valid value throughout for the
	 * same reason as metrics above: every fpm_http_direct_ops_worker_*() call
	 * is a no-op on a NULL ops. */
	struct fpm_http_direct_ops *ops;
	/* issue #339: fpmng_worker_loop_stall_seconds_max. When the previous call
	 * to fpmng_worker_loop() returned, or {0,0} before the first call -- the
	 * sentinel a plain gettimeofday() result can never produce on a machine
	 * with a working clock, since tv_sec would have to be exactly the epoch. */
	struct timeval loop_last_entry;
} fw;

static volatile sig_atomic_t fpm_worker_stopping;

/* issue #339: defined near fpm_worker_memory_bytes() below (it needs that
 * function and fpm_worker_pending_oldest_seconds(), both declared later in
 * this file), but called from fpm_worker_reap() and fpm_worker_accept()
 * above that point -- the same "every removal/creation path funnels through
 * one publish call" shape fpm_worker_metrics_publish() already has. */
static void fpm_worker_ops_publish_live(void);

/* }}} */

/* Configuration ---------------------------------------------------------- */

const char *const fpm_http_direct_worker_rejects[] = {
	FPM_HTTP_DIRECT_REJECTS_COMMON,
	/* The worker script never "ends a request", so the child stays in one
	 * stage for its whole life and these master-side deadlines would either
	 * never fire or kill a healthy worker. Rejected instead of silently
	 * unenforced. */
	"request_terminate_timeout", "request_slowlog_timeout", "slowlog",
	/* Same reason, one step further (issue #59). This executor calls
	 * fpm_request_accepting(false) once for the life of the child, so the
	 * scoreboard has no per-request stage, duration, CPU or peak memory to
	 * report and no request to count. A status page would show one process
	 * stuck in one state and an access log would have nothing to time, so
	 * both are refused rather than answered with placeholders. The classic
	 * executor supports all of these. */
	"pm.status_path", "ping.path", "ping.response",
	"access.log", "access.format", "access.suppress_path",
	/* issue #61. http.max_connections is enforced by keeping the listener
	 * disabled while the worker is at its limit, which is the accept gate of
	 * issue #53 -- deliberately absent from this executor (see the
	 * fpm_http_direct_tls_child_attach() call in child_main). Without it the
	 * only way to honour a limit here is to answer a refusal, and a refused
	 * connection is one no sibling child can pick up: the directive would
	 * mean something different on each executor. http.max_connections_per_client
	 * follows it rather than being half a policy on its own. The first-request
	 * deadline of the same issue IS enforced here -- it needs no gate. */
	"http.max_connections", "http.max_connections_per_client",
	NULL
};

/* How this executor names itself in the startup errors of the shared
 * validation (fpm_http_direct_request.c). */
static const struct fpm_http_direct_labels fpm_worker_labels = {
	.subject = "pool.executor = worker",
	.chdir_note = " (here: the worker script)",
	.type_label = "pool.type = http-direct with pool.executor = worker",
	.script_context = "http-direct worker",
	.script_noun = "the worker script",
};

/* INI value set in the pool, or NULL when only php.ini applies. Same shape and
 * same admin-before-value order as the fiber executor's pool-INI lookup on
 * branch `async` (issue #373); duplicated because that code does not exist on
 * this branch and a stock binary does not contain it. */
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
	const char *timeout_ini;
	zend_long timeout;

	if (fpm_http_direct_validate_common(wp, &fpm_worker_labels) < 0) {
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
	/* issue #331: the ring buffer fw.ready[] is sized from this value once, in
	 * child_main(), so a value that reached the child unchecked would either
	 * allocate nothing (0) or wrap the modulo arithmetic into never accepting
	 * anything -- checked here, at the same master-side validation point as
	 * every other pool.* bound, rather than defended against in the child. */
	if (c->worker_max_pending <= 0) {
		zlog(ZLOG_ALERT, "[pool %s] worker.max_pending(%d) must be a positive value", c->name, c->worker_max_pending);
		return -1;
	}
	/* worker.request_timeout is milliseconds and fpm_conf_set_integer() already
	 * refuses a negative value (see its "greater or equal than zero" message),
	 * so there is nothing left to check here: 0 is the valid "off". */
	/* issue #334: worker.max_memory is bytes, parsed by fpm_conf_set_bytes()
	 * into a size_t -- there is no negative value to reject, the same bar
	 * supervisor.max_memory clears (fpm_pool_supervisor.c has no validation
	 * for it either). worker.max_lifetime is seconds, parsed by
	 * fpm_conf_set_time() into a signed int; clamped to 0 ("off") rather than
	 * rejected outright, the same way fpm_pool_supervisor_validate() clamps a
	 * negative supervisor.max_runtime instead of failing startup over it. */
	if (c->worker_max_lifetime < 0) {
		c->worker_max_lifetime = 0;
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
	/* issue #339: a plain shm increment, safe from a signal handler -- same
	 * reasoning as the fpm_worker_notify() write() call right below, which
	 * this handler already makes. */
	fpm_http_direct_ops_worker_recycle(fw.ops, FPM_WORKER_RECYCLE_SIGNAL);
	fpm_worker_notify();
}

/* Both tables own their values: a worker lives for millions of requests, so a
 * NULL dtor here is a per-request leak, not a shutdown detail. Freeing through
 * the dtor also means every removal path frees exactly once. */
static void fpm_worker_pending_dtor(zval *zv)
{
	struct fpm_worker_pending *p = Z_PTR_P(zv);

	/* issue #335: the only dynamically allocated field this struct owns
	 * besides itself -- released here rather than by every caller of
	 * fpm_worker_reap(), the same "one place frees it" reasoning the comment
	 * above this dtor already gives for the struct itself. */
	if (p->body_cache) {
		zend_string_release(p->body_cache);
	}
	pefree(p, 1);
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
	/* issue #343: an upgraded connection keeps its watchers' ids to be able
	 * to fire them; a freed watcher must not be fired. */
	fpm_ws_unregister_watcher(watcher);
	pefree(watcher, 1);
}

/* Every reply this file hands to libevent goes out through fpm_worker_send_*()
 * so that exactly one place counts what is still unwritten. libevent only
 * queues the bytes on the connection's bufferevent; they reach the socket in a
 * later loop iteration, and evhttp_free() in child_main() frees that
 * bufferevent. A reply produced by the last iteration a worker ever runs is
 * therefore discarded — measured on the test box before this counter existed:
 * with pm.max_requests = 1 the response that trips the limit is lost every
 * time, "curl: (52) Empty reply from server", and the child exits 0 with
 * nothing in the log. fpm_worker_finish_output() waits for these. */
static void fpm_worker_reply_settled(struct evhttp_connection *connection)
{
	if (fw.unflushed) {
		fw.unflushed--;
	}
	if (connection) {
		/* Otherwise the close of a keep-alive connection whose replies all went
		 * out would decrement a second time. */
		evhttp_connection_set_closecb(connection, NULL, NULL);
	}
}

static void fpm_worker_reply_done(struct evhttp_request *http, void *arg)
{
	(void) arg;
	fpm_worker_reply_settled(evhttp_request_get_connection(http));
}

/* A client that aborts mid-write never reaches fpm_worker_reply_done():
 * libevent frees the request from evhttp_connection_free(), which does not
 * invoke on_complete_cb, while evhttp_send_done() does (libevent 2.1.12
 * http.c:2773 vs the request loop in evhttp_connection_free()). Without this
 * the counter would only ever grow, and from the first aborted request on,
 * every shutdown would miss the "nothing unwritten" fast path, block the whole
 * FPM_WORKER_FLUSH_BUDGET, and log a warning about responses that were in fact
 * never owed to anybody. */
static void fpm_worker_reply_aborted(struct evhttp_connection *connection, void *arg)
{
	(void) arg;
	fpm_worker_reply_settled(connection);
}

/* The connection's closecb slot is free for exactly the window this counter
 * covers: every caller clears it immediately before handing the reply over
 * (see fpmng_worker_respond()), and libevent does not associate the next
 * request on a keep-alive connection until on_complete_cb has run
 * (evhttp_send_done()), so at most one counted reply exists per connection. */
static void fpm_worker_count_reply(struct evhttp_request *http)
{
	evhttp_request_set_on_complete_cb(http, fpm_worker_reply_done, NULL);
	evhttp_connection_set_closecb(evhttp_request_get_connection(http), fpm_worker_reply_aborted, NULL);
	fw.unflushed++;
}

static void fpm_worker_send_error(struct evhttp_request *http, int status, const char *reason)
{
	fpm_worker_count_reply(http);
	evhttp_send_error(http, status, reason);
}

static void fpm_worker_send_reply(struct evhttp_request *http, int status, struct evbuffer *body)
{
	fpm_worker_count_reply(http);
	evhttp_send_reply(http, status, NULL, body);
}

static void fpm_worker_reap(struct fpm_worker_pending *p)
{
	zend_hash_index_del(&fw.pending, p->id);
	/* issue #333: the one place every removal path (answered, timed out,
	 * connection closed) funnels through, so this is the only spot that needs
	 * to publish the shrink -- see fpm_http_direct_worker_metrics.h. */
	fpm_worker_metrics_publish(fw.metrics, zend_hash_num_elements(&fw.pending), zend_hash_num_elements(&fw.watchers));
	fpm_worker_ops_publish_live();
}

/* issue #332/#342: this file used to need a "cut a started stream short" path
 * here -- once fpmng_worker_respond_start() has put a status line on the wire
 * there is no way to say "actually, no": evhttp_send_reply() cannot be called a
 * second time, and evhttp_send_error() would try to write a second status line
 * to a connection that already has one. The old answer was classic's
 * fpm_direct_stream_abort() shape (fpm_http_direct.c): shutdown() the
 * connection from inside a callback so the event loop discovers the failure on
 * its next pass and frees the request there. #342 removed the one caller: a
 * worker retiring now ends its streams with evhttp_send_reply_end(), which puts
 * the terminating chunk on the wire and lets an EventSource reconnect -- see
 * fpm_worker_finish_output(). Every other failure a stream can hit (client
 * gone, backpressure, _end()) already had a truthful answer, so nothing calls
 * the abort shape any more and it is gone. */

static void fpm_worker_conn_closed(struct evhttp_connection *connection, void *arg)
{
	struct fpm_worker_pending *p = arg;

	(void) connection;
	/* libevent frees the request together with the connection when it was
	 * already answered outright (fpmng_worker_respond()'s "userdone" case).
	 * Mid-stream -- fpmng_worker_respond_start() called, _end() not yet --
	 * it instead detaches the request from the dying connection
	 * (evhttp_request_get_connection() reads back NULL) and leaves it for
	 * whoever holds the pointer to free; nothing here did, which leaked the
	 * request's headers/uri/input buffer on every stream a client walked away
	 * from mid-flight (issue #332 self-review). evhttp_send_reply_end() on a
	 * request whose connection is already gone just frees it, the same as
	 * fpmng_worker_respond_end() completing normally would have. */
	if (p->http && !evhttp_request_get_connection(p->http)) {
		evhttp_send_reply_end(p->http);
	}
	/* issue #339: closecb is only ever wired to this function while p->http
	 * is still set and no reply has gone out for it yet (fpmng_worker_respond()
	 * and friends clear it before answering, see the comment there) -- so
	 * reaching here with p->http non-NULL means the client hung up on a
	 * request nobody had answered. */
	if (p->http) {
		fpm_http_direct_ops_worker_client_gone(fw.ops);
	}
	if (p->http || p->streaming) {
		/* issue #342: tell userland which request died, rather than letting it
		 * discover one dead client per heartbeat interval -- the next
		 * fpmng_worker_respond_chunk() would return false, but a stream that
		 * emits an event every 15 s should be free to drop its subscription
		 * the moment the client is gone, not one interval later. Recorded for
		 * every unanswered request, streamed or not; the drain is what keeps
		 * the ring bounded. */
		if (fw.closed_max) {
			fw.closed[(fw.closed_head + fw.closed_count) % fw.closed_max] = p->id;
			if (fw.closed_count < fw.closed_max) {
				fw.closed_count++;
			} else {
				fw.closed_head = (fw.closed_head + 1) % fw.closed_max;
			}
		}
	}
	p->http = NULL;
	fpm_worker_notify();
	/* issue #444: a driver that drops the id the moment
	 * fpmng_worker_closed_requests() reports it never responds for it -- the
	 * zombie {streaming = true, http = NULL} entry would then hold its
	 * worker.max_pending slot for the worker's whole life, and the churn of
	 * disconnected subscribers would burn the accept ceiling into 503s and a
	 * forced recycle (confirmed, zero real load). Reap here. Safe because
	 * this callback fires from the event loop, never inside a walk that
	 * matters: the timeout sweep snapshots ids (and tolerates mid-sweep
	 * reaps), and fpm_worker_finish_output()'s un-snapshotted teardown walk
	 * is covered by fw.finishing. The ring record above happens first, so
	 * fpmng_worker_closed_requests() still reports the id. */
	if (!fw.finishing) {
		fpm_worker_reap(p);
	}
}

/* THE ACCEPT CEILING (issue #338)
 *
 * Every child of an http-direct pool accepts from the one listening socket the
 * master opened, and libevent's listener_read_cb() accepts until the queue is
 * empty -- so the child that wakes first takes the whole backlog, and on a
 * keep-alive workload it then owns those connections for their whole life. On
 * pool.executor = classic issue #53 answered that with a gate that stays shut
 * for the whole of a request. That policy cannot transfer verbatim: this
 * executor holds many requests at once by design, so "shut while busy" would be
 * shut for good, which is the reason this file used to install no gate at all.
 *
 * What this executor gets instead is a rate, not a gate: a worker accepts at
 * most worker.accept_threshold connections, and then stops accepting for
 * FPM_WORKER_ACCEPT_COOLDOWN_MS. Two halves, each of them measured rather than
 * assumed (build/benchmark-http-direct-fairness.py --executor worker, 8 workers
 * and 64 keep-alive connections, numbers in docs/http-direct.md):
 *
 *   1. Stop the drain. listener_read_cb() re-checks the listener's enabled flag
 *      after every accept callback, so a callback that disables the listener
 *      ends that drain there. On its own this changes nothing at all: the
 *      backlog is still readable when the listener comes back on the next
 *      fpmng_worker_loop() iteration microseconds later, and the same child
 *      wins the next wakeup too -- measured, the busiest worker's share went
 *      from 0.51 to 0.56, which is noise.
 *   2. Owe the siblings the cooldown. A worker that answers in microseconds is
 *      idle again long before the kernel has scheduled any sibling that the
 *      same connection woke, so without a delay step 1 is a race the incumbent
 *      wins every time. With it, the connections this worker did not take are
 *      still queued for a sibling that is doing nothing. Classic has the same
 *      shape in its 10 ms fpm_direct_tick safety timer.
 *
 * What this deliberately does NOT do is count the requests the worker is
 * holding. An in-flight ceiling was the first thing tried, and it is what
 * classic's gate amounts to -- but here it caps concurrency inside one worker,
 * which is this executor's whole point: with worker.accept_threshold = 1 a
 * single-child pool stopped overlapping its own long requests, and
 * fpmng-http-direct-worker.phpt said so. Fairness between workers must not be
 * bought with concurrency inside one. A worker that is blocked in PHP already
 * stops accepting on its own -- its event loop is not running.
 *
 * worker.accept_threshold = 0 disables all of it, and then no listener state is
 * ever touched and no cooldown timer exists. */
static void fpm_worker_accept_enable(bool on)
{
	struct evconnlistener *l = fw.listener ? evhttp_bound_socket_get_listener(fw.listener) : NULL;

	if (!l) {
		return;
	}
	if (on) {
		evconnlistener_enable(l);
	} else {
		evconnlistener_disable(l);
	}
}

/* Closes the gate once this window's accepts have reached the ceiling, and
 * starts the cooldown that has to pass before it can open again. */
static void fpm_worker_accept_maybe_close(void)
{
	struct timeval tv = {0, FPM_WORKER_ACCEPT_COOLDOWN_MS * 1000};

	if (!fw.accept_threshold || fw.accept_closed ||
		fw.accept_taken < fw.accept_threshold) {
		return;
	}
	fw.accept_closed = true;
	fpm_worker_accept_enable(false);
	/* The timer is the only thing that can end the cooldown, and it is also what
	 * guarantees a worker with nothing else to wait for still wakes up to reopen
	 * its own listener rather than sleeping through the rest of the backlog. */
	fw.accept_cooling = true;
	evtimer_add(fw.accept_cooldown, &tv);
}

/* Reopens once the cooldown has passed. Called at the top of every
 * fpmng_worker_loop() iteration and from the cooldown timer itself, so a worker
 * that stays in PHP across several iterations does not need a timer wakeup to
 * notice that its window is over. */
static void fpm_worker_accept_reopen(void)
{
	if (!fw.accept_closed || fw.accept_cooling) {
		return;
	}
	fw.accept_taken = 0;
	fw.accept_closed = false;
	fpm_worker_accept_enable(true);
}

static void fpm_worker_accept_tick(evutil_socket_t fd, short events, void *arg)
{
	(void) fd;
	(void) events;
	(void) arg;
	fw.accept_cooling = false;
	fpm_worker_accept_reopen();
}

/* evhttp's only per-accepted-connection hook, and therefore the only place
 * the first-request deadline of issue #61 can be armed and the only place this
 * executor can count an accept. Shared by the plain bevcb below and, through
 * fpm_http_direct_tls_child_attach(), by the TLS one -- so the ceiling covers a
 * TLS pool too, and a certificate reload cannot drop it. */
static void fpm_worker_accept_hook(void *arg, struct bufferevent *bev)
{
	(void) arg;
	fpm_http_direct_conns_accepted(fw.conns, bev);
	fw.accept_taken++;
	/* issue #339: fpmng_pool_worker_accepted_total -- the fairness measurement
	 * the accept ceiling above exists to protect, counted at the same point
	 * the classic executor's fpm_direct_accept_close_gate() counts its own
	 * conn_accepted. */
	fpm_http_direct_ops_accepted(fw.ops);
	fpm_worker_accept_maybe_close();
}

static struct bufferevent *fpm_worker_bevcb(struct event_base *base, void *arg)
{
	struct bufferevent *bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);

	fpm_worker_accept_hook(arg, bev);
	return bev;
}

static void fpm_worker_accept(struct evhttp_request *http, void *arg)
{
	struct fpm_worker_pending *p;
	char *peer = NULL;
	ev_uint16_t peer_port = 0;

	(void) arg;
	/* The request arrived, so its deadline is spent -- before the ACL and the
	 * saturation check below, because both of those end the request and the
	 * connection they end must not still be holding an armed timer. The return
	 * value cannot be anything but 0 here: it reports
	 * http.max_connections_per_client, which this executor rejects. */
	(void) fpm_http_direct_conns_request(fw.conns,
		evhttp_connection_get_bufferevent(evhttp_request_get_connection(http)), NULL, NULL);
	/* Before the saturation check below: a client that may not be here learns
	 * nothing about how busy the worker is. */
	if (fw.acl) {
		/* Not in an on-accept hook: libevent's bevcb runs before the peer
		 * address is known, so the first place this can be asked is here --
		 * the same place the http gateway asks it. */
		evhttp_connection_get_peer(evhttp_request_get_connection(http), &peer, &peer_port);
		if (!peer || !fpm_http_acl_check(fw.acl, peer)) {
			fpm_http_direct_ops_worker_refused(fw.ops, FPM_WORKER_REFUSED_ACL);
			fpm_worker_send_error(http, 403, "Forbidden");
			return;
		}
	}
	if (fpm_worker_stopping || fw.ready_count >= fw.ready_max ||
		zend_hash_num_elements(&fw.pending) >= fw.ready_max) {
		fpm_http_direct_ops_worker_refused(fw.ops,
			fpm_worker_stopping ? FPM_WORKER_REFUSED_STOPPING : FPM_WORKER_REFUSED_SATURATED);
		/* No "Connection: close" of our own: evhttp_send_error() clears the
		 * output headers and sets it itself. */
		fpm_worker_send_error(http, 503, "Worker unavailable");
		/* Saturation must not be permanent. A pending entry is removed only by
		 * fpmng_worker_respond() or, once it expires, by
		 * fpm_worker_sweep_expired() -- so a handler that returns without
		 * answering AND never times out (worker.request_timeout = 0, still the
		 * default) burns its slot for the life of the worker; after
		 * worker.max_pending such leaks every later request would get 503
		 * forever, and nothing would notice — request_terminate_timeout is
		 * rejected by this executor and pm.max_requests only counts answered
		 * requests. Ask for a graceful stop instead: in-flight work drains,
		 * the child exits, the master respawns it. A genuine burst of more
		 * than worker.max_pending concurrent requests therefore recycles the
		 * worker as well; that is the same drain pm.max_requests performs,
		 * and the pool is already answering 503 at that point. The cost of
		 * that choice -- worker.max_pending deliberately held long-polls are
		 * lost with the worker -- is the documented contract of this mode,
		 * not an accident: see "Held requests under pool.executor = worker" in
		 * docs/http-direct.md (issue #184), and worker.request_timeout
		 * (issue #331) for the lever that keeps a stuck handler from reaching
		 * this ceiling at all. */
		if (!fpm_worker_stopping) {
			zlog(ZLOG_WARNING, "[pool %s] http-direct worker: %u requests accepted but unanswered; "
				"answering 503 and asking the worker script to stop so the master can respawn it",
				fw.wp->config->name, fw.ready_max);
			fpm_worker_stopping = 1;
			fpm_http_direct_ops_worker_recycle(fw.ops, FPM_WORKER_RECYCLE_SATURATION);
			fpm_worker_notify();
		}
		return;
	}
	if (!fpm_http_direct_request_acceptable(http)) {
		fpm_http_direct_ops_worker_refused(fw.ops, FPM_WORKER_REFUSED_BAD_REQUEST);
		fpm_worker_send_error(http, 400, "Bad request");
		return;
	}
	p = pemalloc(sizeof(*p), 1);
	p->http = http;
	p->id = fw.next_id++;
	/* pemalloc() does not zero: every fresh entry starts life not streaming,
	 * set explicitly rather than left to whatever garbage this allocation
	 * happened to contain (issue #332). Same for body_cache (issue #335): NULL
	 * until fpmng_worker_request_body()'s first call for this id. */
	p->streaming = false;
	p->body_cache = NULL;
	gettimeofday(&p->accepted_at, NULL);
	zend_hash_index_add_new_ptr(&fw.pending, p->id, p);
	/* issue #333: see the matching call in fpm_worker_reap(). */
	fpm_worker_metrics_publish(fw.metrics, zend_hash_num_elements(&fw.pending), zend_hash_num_elements(&fw.watchers));
	fw.ready[(fw.ready_head + fw.ready_count) % fw.ready_max] = p->id;
	/* issue #339: fw.ready_count itself changes right below, so the publish
	 * happens after it -- see fpm_worker_ops_publish_live()'s use of
	 * fw.ready_count for fpmng_pool_worker_queued. */
	fw.ready_count++;
	fpm_worker_ops_publish_live();
	/* evhttp serves one request per connection at a time, so at most one
	 * pending request per connection can be waiting for a close notice. */
	evhttp_connection_set_closecb(evhttp_request_get_connection(http), fpm_worker_conn_closed, p);
	fpm_worker_notify();
}

static bool fpm_worker_flush_expired;

static void fpm_worker_flush_deadline(evutil_socket_t fd, short events, void *arg)
{
	(void) fd;
	(void) events;
	(void) arg;
	fpm_worker_flush_expired = true;
	event_base_loopbreak(fw.base);
}

/* The last thing child_main() does with the transport, and the only place that
 * enforces the invariant every client is owed: an accepted connection is never
 * closed without a response that actually reached the socket. Two distinct
 * ways to break it meet here.
 *
 * One is a request nobody answered — a bridge that concluded it had drained
 * from its own in-flight counter while the SAPI still had a queued request
 * (task 080), a handler that threw, an exit() in the worker script. Those get
 * a 503 here, in the SAPI, so third-party bridges are covered too.
 *
 * The other is a response that was answered but never written. libevent only
 * queues a reply on the connection's bufferevent, and evhttp_free() below
 * frees it: a reply produced in the last loop iteration a worker ever runs is
 * discarded. Measured on the test box against this file before fw.unflushed
 * existed, with pm.max_requests = 1 — both requests of a two-request run got
 * "curl: (52) Empty reply from server", the child exited 0 and the log said
 * only "child exited with code 0". Driving the base until the replies complete
 * is what the classic transport does for the same reason, counting them in
 * w->pending and refusing to break its loop until they are done
 * (fpm_http_direct.c:543 counts one in, :437 refuses the loopbreak until the
 * count is back to zero).
 *
 * The wait is bounded: a client that stopped reading must not keep the child
 * alive, and the master is already timing this shutdown. */
static void fpm_worker_finish_output(void)
{
	struct fpm_worker_pending *p;
	unsigned abandoned = 0;
	unsigned streams_ended = 0;
	struct event *deadline;
	struct timeval budget = {FPM_WORKER_FLUSH_BUDGET, 0};

	/* No new connection may join the set we are about to answer. A request
	 * arriving on an already-open keep-alive connection still can, and
	 * fpm_worker_accept() answers it 503 itself now that stopping is set. */
	if (fw.listener) {
		evhttp_del_accept_socket(fw.http, fw.listener);
		fw.listener = NULL;
	}
	fpm_worker_stopping = 1;

	ZEND_HASH_FOREACH_PTR(&fw.pending, p) {
		if (!p->http) {
			continue;	/* the client is already gone: fpm_worker_conn_closed() cleared it */
		}
		if (p->streaming) {
			/* issue #342: unlike the status line, the terminator does not have
			 * to be the end of the world -- evhttp_send_reply_end() puts the
			 * final zero-length chunk on the wire, which is exactly what an
			 * EventSource needs to notice a clean close and reconnect with
			 * Last-Event-ID. Cutting the stream short here (the #332 behaviour,
			 * fpm_worker_stream_abort()) turned every worker retirement into a
			 * transport error for every subscriber. The worker script is gone
			 * by now, so ending on its behalf is safe: nobody else can be
			 * writing to this request. The entry is not reaped -- the caller
			 * walks fw.pending, and zend_hash_destroy() frees it during
			 * teardown. */
			evhttp_connection_set_closecb(evhttp_request_get_connection(p->http), NULL, NULL);
			fpm_worker_count_reply(p->http);
			evhttp_send_reply_end(p->http);
			p->http = NULL;
			streams_ended++;
			continue;
		}
		/* Same shape as the saturation 503 in fpm_worker_accept(), including
		 * dropping the close callback first: libevent owns and frees the
		 * request from here on, and a close notice must not reach a pending
		 * entry we are abandoning. */
		evhttp_connection_set_closecb(evhttp_request_get_connection(p->http), NULL, NULL);
		/* No "Connection: close" of our own: evhttp_send_error() clears the
		 * output headers (see fpmng_worker_respond()) and sets it itself. */
		fpm_worker_send_error(p->http, 503, "Worker unavailable");
		p->http = NULL;
		abandoned++;
	} ZEND_HASH_FOREACH_END();

	if (abandoned) {
		zlog(ZLOG_WARNING, "[pool %s] http-direct worker: the worker script stopped with %u accepted "
			"request(s) unanswered; answering 503 rather than closing the connection silently",
			fw.wp->config->name, abandoned);
		/* issue #339: fpmng_pool_worker_abandoned_total. Not streams_cut_short
		 * above -- those got a status line (possibly some chunks) before the
		 * worker stopped, which is an answer, just an incomplete one; this
		 * counter is for requests that got no answer at all. */
		fpm_http_direct_ops_worker_abandoned(fw.ops, abandoned);
	}
	if (streams_ended) {
		zlog(ZLOG_NOTICE, "[pool %s] http-direct worker: the worker script stopped with %u streamed "
			"response(s) still in progress; those were ended with their terminating chunk so the "
			"clients can reconnect (issue #342)",
			fw.wp->config->name, streams_ended);
	}
	if (!fw.unflushed) {
		return;
	}
	fpm_worker_flush_expired = false;
	deadline = evtimer_new(fw.base, fpm_worker_flush_deadline, NULL);
	if (deadline && evtimer_add(deadline, &budget) == 0) {
		/* EVLOOP_ONCE blocks, so the deadline timer is what guarantees this
		 * returns; a non-zero result means libevent has nothing left to wait
		 * on, which for our purposes is also "done". */
		while (fw.unflushed && !fpm_worker_flush_expired &&
			event_base_loop(fw.base, EVLOOP_ONCE) == 0) {
		}
	}
	if (deadline) {
		event_free(deadline);
	}
	if (fw.unflushed) {
		zlog(ZLOG_WARNING, "[pool %s] http-direct worker: %u response(s) were still unwritten after %d s; "
			"those connections are closed without a reply",
			fw.wp->config->name, fw.unflushed, FPM_WORKER_FLUSH_BUDGET);
		/* issue #339: also fpmng_pool_worker_abandoned_total -- the client was
		 * told an answer was coming (evhttp_send_reply() already queued it)
		 * but it never reached the wire, which from the client's side is
		 * indistinguishable from never having been answered at all. */
		fpm_http_direct_ops_worker_abandoned(fw.ops, fw.unflushed);
	}
}

/* worker.request_timeout (issue #331): fw.request_timeout_sweep's callback,
 * armed only when the directive is non-zero. Runs every
 * FPM_WORKER_REQUEST_TIMEOUT_SWEEP_DIVISOR'th of the timeout and answers 504
 * to any pending entry whose accepted_at is old enough, then reaps it exactly
 * like fpmng_worker_respond() does on the happy path.
 *
 * ids are snapshotted first, the same defensive shape
 * fpm_worker_activate_buffered() uses for fw.watchers: fpm_worker_send_error()
 * calls evhttp_send_error(), which can run this same event base's other
 * pending callbacks synchronously in some libevent versions' bufferevent
 * paths, so a plain ZEND_HASH_FOREACH over fw.pending while reaping entries
 * out of it under the same walk would be modifying the table the iterator is
 * still holding. */
static void fpm_worker_sweep_expired(evutil_socket_t fd, short events, void *arg)
{
	struct timeval now;
	zend_ulong *ids;
	uint32_t count = 0, i;
	zend_ulong id;
	unsigned expired = 0;

	(void) fd;
	(void) events;
	(void) arg;

	if (zend_hash_num_elements(&fw.pending) == 0) {
		return;
	}
	gettimeofday(&now, NULL);
	ids = safe_emalloc(zend_hash_num_elements(&fw.pending), sizeof(zend_ulong), 0);
	ZEND_HASH_FOREACH_NUM_KEY(&fw.pending, id) {
		ids[count++] = id;
	} ZEND_HASH_FOREACH_END();

	for (i = 0; i < count; i++) {
		struct fpm_worker_pending *p = zend_hash_index_find_ptr(&fw.pending, ids[i]);
		long elapsed_ms;

		/* Gone already (answered, closed, or reaped by an earlier iteration of
		 * this same sweep) -- matches how fpm_worker_conn_closed()'s NULL
		 * leaves an entry for a later reader to skip rather than assuming it
		 * is still live. */
		if (!p || !p->http) {
			continue;
		}
		/* issue #332: worker.request_timeout means "never got its first byte
		 * of response", not "streaming took a while" -- a stream can
		 * legitimately run for as long as the client keeps reading. Once
		 * fpmng_worker_respond_start() has run, this sweep has nothing left
		 * to bound: the response side is now the client's http.read_timeout
		 * and worker.send_buffer_limit's backpressure, not this timer. */
		if (p->streaming) {
			continue;
		}
		elapsed_ms = (now.tv_sec - p->accepted_at.tv_sec) * 1000L +
			(now.tv_usec - p->accepted_at.tv_usec) / 1000L;
		if (elapsed_ms < fw.wp->config->worker_request_timeout) {
			continue;
		}
		/* Same shape as the saturation and shutdown 503 paths: drop the close
		 * callback before handing the request to libevent, since a later
		 * close on this connection must not reach a pending entry we are
		 * about to reap. */
		evhttp_connection_set_closecb(evhttp_request_get_connection(p->http), NULL, NULL);
		fpm_worker_send_error(p->http, 504, "Gateway Timeout");
		p->http = NULL;
		fpm_worker_reap(p);
		expired++;
		/* fw.ready still holds this id -- reclaimed the next time
		 * fpmng_worker_next_request() dequeues it and finds nothing in
		 * fw.pending, the same lazy path an already-closed connection's id
		 * takes there. worker.max_pending's saturation check ORs fw.ready_count
		 * with zend_hash_num_elements(&fw.pending), and the reap above already
		 * shrank the latter, so a timed-out request stops counting against the
		 * ceiling as soon as its id is drained -- not held forever the way an
		 * unanswered, non-expiring leak would be. */
	}
	efree(ids);
	if (expired) {
		/* Not permanent: unlike the worker.max_pending 503 path above, a
		 * timeout answers and frees exactly the requests that overstayed --
		 * everything else keeps running, so the worker is not asked to stop. */
		zlog(ZLOG_WARNING, "[pool %s] http-direct worker: %u request(s) exceeded worker.request_timeout(%d ms); "
			"answering 504 Gateway Timeout",
			fw.wp->config->name, expired, fw.wp->config->worker_request_timeout);
	}
}

/* This process's own peak resident set size, in bytes, for worker.max_memory
 * (issue #334). Identical to fpm_pool_supervisor_memory_bytes()
 * (fpm_pool_supervisor.c, issue #324) -- not shared between the two files
 * because this executor and the supervisor pool type do not otherwise share
 * any code, and the whole function is three lines wrapping one syscall.
 * getrusage(RUSAGE_SELF).ru_maxrss is a monotonically non-decreasing
 * high-water mark -- exactly what a one-way "has this process outgrown its
 * budget" check needs -- and its unit differs by platform: Linux (the target
 * platform) reports kilobytes, Darwin/macOS (a local dev build) reports
 * bytes. Both are converted to bytes here so worker.max_memory (parsed by
 * fpm_conf_set_bytes(), also bytes) never has to know which platform it is
 * running on. */
static size_t fpm_worker_memory_bytes(void)
{
	struct rusage ru;

	if (getrusage(RUSAGE_SELF, &ru) != 0) {
		return 0;
	}
#ifdef __APPLE__
	return (size_t) ru.ru_maxrss;
#else
	return (size_t) ru.ru_maxrss * 1024;
#endif
}

/* issue #339: the oldest fw.pending entry's age, for
 * fpmng_pool_worker_pending_oldest_seconds. Same walk and the same
 * "!p->http || p->streaming" skip as fpm_worker_sweep_expired() above -- a
 * streaming or already-answered entry has no bearing on "how long has this
 * worker made someone wait for a first byte". Returns 0 when nothing
 * qualifies (the common case), which the caller renders as "no oldest
 * request" rather than a fake handful of nanoseconds. */
static double fpm_worker_pending_oldest_seconds(void)
{
	struct timeval now;
	struct fpm_worker_pending *p;
	double oldest = 0;

	if (zend_hash_num_elements(&fw.pending) == 0) {
		return 0;
	}
	gettimeofday(&now, NULL);
	ZEND_HASH_FOREACH_PTR(&fw.pending, p) {
		double age;

		if (!p->http || p->streaming) {
			continue;
		}
		age = (double) (now.tv_sec - p->accepted_at.tv_sec) +
			(double) (now.tv_usec - p->accepted_at.tv_usec) / 1000000.0;
		if (age > oldest) {
			oldest = age;
		}
	} ZEND_HASH_FOREACH_END();
	return oldest;
}

/* issue #339: publishes every fpmng_pool_worker_* gauge in one call, so the
 * four call sites below (accept, reap, event create/free) cannot let one
 * gauge drift out of step with another the way four separate publish
 * functions could. fw.ops is NULL-safe throughout, same contract as
 * fpm_worker_metrics_publish(). */
static void fpm_worker_ops_publish_live(void)
{
	struct fpm_http_direct_ops_worker_live live;
	struct fpm_worker_watcher *watcher;

	if (!fw.ops) {
		return;
	}
	memset(&live, 0, sizeof(live));
	live.queued = fw.ready_count;
	live.pending_oldest_seconds = fpm_worker_pending_oldest_seconds();
	ZEND_HASH_FOREACH_PTR(&fw.watchers, watcher) {
		switch (watcher->type) {
			case FPM_WORKER_EV_READ:
				live.watchers_read++;
				break;
			case FPM_WORKER_EV_WRITE:
				live.watchers_write++;
				break;
			case FPM_WORKER_EV_TIMER:
				live.watchers_timer++;
				break;
		}
	} ZEND_HASH_FOREACH_END();
	live.memory_bytes = fpm_worker_memory_bytes();
	live.unflushed = fw.unflushed;
	fpm_http_direct_ops_worker_publish(fw.ops, &live);
}

/* worker.max_memory/worker.max_lifetime (issue #334): fw.health_sweep's
 * callback, armed whenever either directive is non-zero (see the field
 * comment on FPM_WORKER_HEALTH_SWEEP_INTERVAL_SEC). Trips the exact same
 * graceful stop pm.max_requests uses -- fpm_worker_stopping = 1, notify, let
 * the worker script drain and exit on its own -- never a kill: this executor
 * has no per-request state isolation, so a kill mid-flight would lose
 * whatever the worker was holding, exactly the outcome worker.request_timeout
 * and worker.max_pending's own drains already avoid. */
static void fpm_worker_health_sweep(evutil_socket_t fd, short events, void *arg)
{
	size_t memory_bytes;
	long lifetime_sec;

	(void) fd;
	(void) events;
	(void) arg;

	if (fpm_worker_stopping) {
		return;
	}
	if (fw.wp->config->worker_max_memory > 0) {
		memory_bytes = fpm_worker_memory_bytes();
		if (memory_bytes >= fw.wp->config->worker_max_memory) {
			zlog(ZLOG_NOTICE, "[pool %s] http-direct worker: memory usage %zu bytes reached "
				"worker.max_memory = %zu bytes, recycling the worker",
				fw.wp->config->name, memory_bytes, fw.wp->config->worker_max_memory);
			fpm_worker_stopping = 1;
			fpm_http_direct_ops_worker_recycle(fw.ops, FPM_WORKER_RECYCLE_MAX_MEMORY);
			fpm_worker_notify();
			return;
		}
	}
	if (fw.wp->config->worker_max_lifetime > 0) {
		lifetime_sec = (long) (time(NULL) - fw.start_time);
		if (lifetime_sec >= fw.wp->config->worker_max_lifetime) {
			zlog(ZLOG_NOTICE, "[pool %s] http-direct worker: lifetime %ld s reached "
				"worker.max_lifetime = %d s, recycling the worker",
				fw.wp->config->name, lifetime_sec, fw.wp->config->worker_max_lifetime);
			fpm_worker_stopping = 1;
			fpm_http_direct_ops_worker_recycle(fw.ops, FPM_WORKER_RECYCLE_MAX_LIFETIME);
			fpm_worker_notify();
		}
	}
}

/* PHP's own stream_select() re-casts every stream on every call
 * (ext/standard/streamsfuncs.c:673), and for an SSL stream that cast is not a
 * passive lookup: while the read buffer is empty it pulls SSL_pending() bytes
 * out of OpenSSL into it (ext/openssl/xp_ssl.c, case
 * PHP_STREAM_AS_FD_FOR_SELECT). fpmng_worker_event_create() casts once and
 * keeps the descriptor, so bytes that arrived inside a TLS record burst used to
 * be invisible for ever — the descriptor is empty, no readability event will
 * ever fire again, and the application waits on it with nothing logged. Task
 * 075 measured it: over keep-alive TLS with one read per readable event, 769 of
 * 8192 body bytes stranded at a 1 KiB read chunk, 3841 at 4 KiB, 7937 at 8 KiB.
 * Matching PHP's default chunk_size does not help, because the stranded amount
 * is whatever the peer happened to send. */
static void fpm_worker_stream_refill(php_stream *stream)
{
	int fd = -1;	/* php_stream_cast() writes an int for PHP_STREAM_AS_FD_FOR_SELECT */

	/* show_err = 0: a stream that cannot be cast is not an error on this path,
	 * it simply has nothing we can look at. The returned descriptor is
	 * deliberately dropped — fpmng_worker_event_create() owns the one libevent
	 * watches, and this call is made for its side effect. */
	(void) php_stream_cast(stream, PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL,
		(void *) &fd, 0);
}

/* Not stream->has_buffered_data (main/php_streams.h:215): despite the name it is
 * cleared at the end of php_stream_read() (main/streams/streams.c:685), so from
 * outside it says nothing. The buffer itself does. */
static size_t fpm_worker_stream_peek_buffered(php_stream *stream)
{
	return stream->writepos > stream->readpos ? (size_t) (stream->writepos - stream->readpos) : 0;
}

static size_t fpm_worker_stream_buffered(php_stream *stream)
{
	if (!stream) {
		return 0;
	}
	fpm_worker_stream_refill(stream);
	return fpm_worker_stream_peek_buffered(stream);
}

/* Not php_stream_from_zval_no_verify(): that macro throws a TypeError for a
 * resource whose type zend_resource_dtor() has already reset to -1, which is
 * exactly the state of a stream userland fclose()d while its watcher was still
 * registered (Zend/zend_list.c). Thrown from fpm_worker_activate_buffered() the
 * exception would leave fpmng_worker_loop() through RETURN_THROWS() before
 * event_base_loop() ever ran, and because the watcher stays registered and
 * pending, every later call would throw again — the worker's loop dead for
 * good, blaming an argument the caller never passed. Passing NULL for the
 * resource type name makes zend_fetch_resource2_ex() return NULL quietly
 * instead. */
static php_stream *fpm_worker_watcher_stream(struct fpm_worker_watcher *watcher)
{
	if (Z_ISUNDEF(watcher->stream)) {
		return NULL;
	}
	return (php_stream *) zend_fetch_resource2_ex(&watcher->stream, NULL,
		php_file_le_stream(), php_file_le_pstream());
}

/* Runs once per fpmng_worker_loop() iteration, before libevent gets a chance to
 * sleep. A read watcher whose stream holds userland-buffered bytes is activated
 * by hand: libevent then dispatches it in this iteration and computes a zero
 * timeout, so the $blocking argument stops being able to park the worker on a
 * descriptor that will never be readable again.
 *
 * Written against ids rather than as a single hash walk because the refill is
 * not guaranteed to be passive: for a user-space stream php_stream_cast() calls
 * the userland stream_cast() method (main/streams/userspace.c,
 * php_userstreamop_cast()), and such a stream reaches this loop whenever that
 * method hands back a real socket. From there userland can call
 * fpmng_worker_event_free() — freeing the very watcher a ZEND_HASH_FOREACH would
 * still be holding — or fpmng_worker_event_create(), which can reallocate the
 * table under the iterator. So: snapshot the ids, and look the watcher up again
 * after every call that might have re-entered PHP. */
static void fpm_worker_activate_buffered(void)
{
	zend_ulong *ids;
	uint32_t count = 0, i;
	zend_ulong id;

	if (zend_hash_num_elements(&fw.watchers) == 0) {
		return;
	}
	ids = safe_emalloc(zend_hash_num_elements(&fw.watchers), sizeof(zend_ulong), 0);
	ZEND_HASH_FOREACH_NUM_KEY(&fw.watchers, id) {
		ids[count++] = id;
	} ZEND_HASH_FOREACH_END();

	for (i = 0; i < count; i++) {
		struct fpm_worker_watcher *watcher = zend_hash_index_find_ptr(&fw.watchers, ids[i]);
		php_stream *stream;
		size_t before, buffered;

		/* Timers carry no stream; write watchers are a different question
		 * (buffering on the write side is out of scope for task 079); an event
		 * that is not pending was disabled by userland and must stay silent. */
		if (!watcher || !(event_get_events(watcher->ev) & EV_READ) ||
			!event_pending(watcher->ev, EV_READ, NULL)) {
			continue;
		}
		stream = fpm_worker_watcher_stream(watcher);
		if (!stream) {
			continue;
		}
		/* Sampled before the refill, and that is the whole point of taking two
		 * samples: it is the only place the consumer's own progress shows.
		 * Comparing two post-refill sizes would call a perfectly behaved
		 * consumer stuck, because on a stream with steady inbound TLS the
		 * refill puts back what was just drained and the size never appears to
		 * shrink (ext/openssl/xp_ssl.c refills exactly when the buffer is
		 * empty). */
		before = fpm_worker_stream_peek_buffered(stream);

		fpm_worker_stream_refill(stream);

		/* Re-looked-up, and the stream re-fetched from it: the refill above may
		 * have run userland that freed either. */
		watcher = zend_hash_index_find_ptr(&fw.watchers, ids[i]);
		if (!watcher || !event_pending(watcher->ev, EV_READ, NULL)) {
			continue;
		}
		stream = fpm_worker_watcher_stream(watcher);
		if (!stream) {
			continue;
		}
		buffered = fpm_worker_stream_peek_buffered(stream);
		if (!buffered) {
			/* Nothing to activate, so nothing to spin on either. */
			watcher->spin = 0;
			watcher->spin_warned = false;
			watcher->buffered = 0;
			continue;
		}
		if (before < watcher->buffered) {
			watcher->spin = 0;
			watcher->spin_warned = false;
		} else if (++watcher->spin >= FPM_WORKER_SPIN_LIMIT && !watcher->spin_warned) {
			watcher->spin_warned = true;
			zlog(ZLOG_WARNING, "[pool %s] http-direct worker: a read watcher was invoked %d times in a row "
				"with %zu byte(s) left in the stream's userland buffer and nothing consuming them, "
				"so the loop cannot sleep. Read until the read comes up short, not once per readable event",
				fw.wp->config->name, FPM_WORKER_SPIN_LIMIT, buffered);
		}
		watcher->buffered = buffered;
		event_active(watcher->ev, EV_READ, 0);
	}
	efree(ids);
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

/* ------------------------------------------------------------------ issue #343 */

/* fpmng_worker_upgrade(): the one builtin of the WebSocket story. The worker
 * executor runs PHP in the process that owns the accepted connection's
 * bufferevent, so a connection can leave evhttp's request/response world and
 * become an ordinary bidirectional PHP stream -- RFC 6455 framing, masking,
 * ping/pong and close codes stay in userland codecs (amphp/websocket-server,
 * ratchet/rfc6455, ReactPHP), exactly as this executor keeps Revolt/amphp
 * out of C. The classic executor keeps all three of #68's impossibility
 * policies: it has no event loop of its own to hand a hijacked connection to.
 *
 * The hijack follows libevent 2.2's own evws_new_session() (ws.c) in intent,
 * on the public 2.1 API -- and where 2.2's internal evhttp_start_ws_() steals
 * evcon->bufev and frees the connection, the public API can reach neither
 * evcon->bufev nor the connection list, and 2.1's own
 * evhttp_connection_free() shutdown(SHUT_WR)s the fd on the way out, which
 * would kill the WebSocket the moment it was born. So the connection stays
 * evhttp's for the child's whole life: the hijack writes the 101 into the
 * bufferevent, replaces ALL of its callbacks (evhttp's state machine on this
 * connection is never driven again), clears the closecb and the timeouts, and
 * leaves the request exactly where evhttp put it -- never answered, never
 * driven, freed with the connection at evhttp_free(). The stream's close
 * does shutdown(SHUT_RDWR) -- what the client sees -- and the registry below
 * (fpm_ws_all, walked in child_main() just before that free) tells a late
 * stream close the bufferevent is already gone. The cost, honestly stated:
 * one bufferevent plus one request per WebSocket connection ever accepted
 * stays allocated until the worker recycles (pm.max_requests,
 * worker.max_lifetime, the master's own recycling), the same bound every
 * other per-connection allocation of this executor lives under.
 *
 * Readiness: an fd watcher alone cannot see this connection's data -- the
 * bufferevent drains the descriptor into its input evbuffer, and a drained
 * descriptor never fires. The bufferevent's own read/write callbacks
 * therefore activate the read/write watchers bound to this context
 * (watcher->ws, wired in fpmng_worker_event_create()), which is also what
 * makes fpmng_worker_stream_has_buffered() and feof() honest: fpmng stream
 * ops answer from the evbuffers, and the event callback flags EOF on the
 * stream the moment the peer goes away. */

struct fpm_ws_ctx {
	struct bufferevent *bev;
	php_stream *stream;
	bool eof;			/* the event callback saw BEV_EVENT_EOF or BEV_EVENT_ERROR */
	bool orphaned;		/* teardown: the bufferevent was (or is being) freed by evhttp_free() */
	/* watcher ids bound to this connection, fired by the bufferevent
	 * callbacks; ids only -- the table re-lookup and the ->ws check happen at
	 * fire time, the same shape fpm_worker_activate_buffered() uses. */
	zend_ulong *watchers;
	unsigned watchers_n;
	unsigned watchers_cap;
	struct fpm_ws_ctx *next;
};

/* Every connection this child has hijacked and not yet closed, so the
 * teardown in child_main() can tell the stream close ops that evhttp_free()
 * is about to free the bufferevents it never let go of. The head is touched
 * only from this child's own single-threaded callbacks. */
static struct fpm_ws_ctx *fpm_ws_all = NULL;

static void fpm_ws_unregister(struct fpm_ws_ctx *ctx)
{
	struct fpm_ws_ctx **p = &fpm_ws_all;

	while (*p && *p != ctx) {
		p = &(*p)->next;
	}
	if (*p) {
		*p = ctx->next;
	}
}

static void fpm_ws_unregister_watcher(struct fpm_worker_watcher *watcher)
{
	struct fpm_ws_ctx *ctx = watcher->ws;
	unsigned i;

	watcher->ws = NULL;
	if (!ctx) {
		return;
	}
	for (i = 0; i < ctx->watchers_n; i++) {
		if (ctx->watchers[i] == watcher->id) {
			ctx->watchers[i] = ctx->watchers[--ctx->watchers_n];
			return;
		}
	}
}

/* The watcher callbacks run PHP, so every fire needs the same guard
 * fpm_worker_activate_buffered() and fpm_worker_watcher_fire() use: the
 * callback may free watchers (event_free inside a callback is the advertised
 * cancellation idiom), so ids are snapshotted and each is re-looked-up and
 * re-checked before event_active() -- which only arms the event for THIS
 * iteration, exactly what a real fd readability would have done. */
static void fpm_ws_fire(struct fpm_ws_ctx *ctx, short type)
{
	zend_ulong *ids;
	unsigned n = ctx->watchers_n, i;

	if (!n) {
		return;
	}
	ids = emalloc(n * sizeof(*ids));
	memcpy(ids, ctx->watchers, n * sizeof(*ids));
	for (i = 0; i < n; i++) {
		struct fpm_worker_watcher *watcher = zend_hash_index_find_ptr(&fw.watchers, ids[i]);

		if (!watcher || watcher->ws != ctx || watcher->type != type
			|| !event_pending(watcher->ev, type == FPM_WORKER_EV_READ ? EV_READ : EV_WRITE, NULL)) {
			continue;
		}
		event_active(watcher->ev, type == FPM_WORKER_EV_READ ? EV_READ : EV_WRITE, 0);
	}
	efree(ids);
}

static void fpm_ws_readcb(struct bufferevent *bev, void *arg)
{
	(void) bev;
	fpm_ws_fire(arg, FPM_WORKER_EV_READ);
}

static void fpm_ws_writecb(struct bufferevent *bev, void *arg)
{
	(void) bev;
	fpm_ws_fire(arg, FPM_WORKER_EV_WRITE);
}

static void fpm_ws_eventcb(struct bufferevent *bev, short what, void *arg)
{
	struct fpm_ws_ctx *ctx = arg;

	(void) bev;
	if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
		ctx->eof = true;
		/* The userspace answer to "did the peer go away": the next fread()
		 * comes up empty and feof() is true. Setting the flag on the stream
		 * here (rather than in the read op) is what makes feof() exact -- a
		 * merely empty buffer must not read as EOF. */
		if (ctx->stream) {
			ctx->stream->eof = 1;
		}
		fpm_ws_fire(ctx, FPM_WORKER_EV_READ);
	}
}

static ssize_t fpm_ws_read(php_stream *stream, char *buf, size_t count)
{
	struct fpm_ws_ctx *ctx = stream->abstract;
	struct evbuffer *in;
	size_t buffered;
	size_t take;

	if (ctx->orphaned) {
		/* Teardown already freed the bufferevent (#443): answer like a dead
		 * stream, never touch it. */
		return 0;
	}
	in = bufferevent_get_input(ctx->bev);
	buffered = evbuffer_get_length(in);

	if (count > buffered) {
		count = buffered;
	}
	/* 0 = "nothing buffered" -- _php_stream_read() does not treat a 0 from
	 * the ops read as EOF, so feof() stays exactly as the event callback
	 * flags it. An empty buffer on this stream is the NORMAL state between
	 * WebSocket messages, not an error and not an end. */
	take = count ? (size_t) evbuffer_remove(in, buf, count) : 0;
	return (ssize_t) take;
}

static ssize_t fpm_ws_write(php_stream *stream, const char *buf, size_t count)
{
	struct fpm_ws_ctx *ctx = stream->abstract;
	struct evbuffer *out;

	if (ctx->orphaned) {
		/* The bufferevent is gone (#443): report the write failed rather
		 * than queue bytes into freed heap. */
		return -1;
	}
	out = bufferevent_get_output(ctx->bev);

	/* #332's backpressure contract, on the connection's own output evbuffer:
	 * once what is queued-but-unwritten is at the limit, refuse to queue more
	 * and return 0 -- the write watcher (fired by fpm_ws_writecb() when the
	 * buffer drains) is what wakes the codec to try again. */
	if (fw.wp->config->worker_send_buffer_limit > 0
		&& evbuffer_get_length(out) >= (size_t) fw.wp->config->worker_send_buffer_limit) {
		return 0;
	}
	return bufferevent_write(ctx->bev, buf, count) == 0 ? (ssize_t) count : -1;
}

static void fpm_ws_close_after_write(struct bufferevent *bev, short what, void *arg);
static void fpm_ws_close_after_write_cb(struct bufferevent *bev, void *arg);

static int fpm_ws_close(php_stream *stream, int close_handle)
{
	struct fpm_ws_ctx *ctx = stream->abstract;
	zend_ulong *ids;
	unsigned n, i;

	(void) close_handle;
	fpm_ws_unregister(ctx);
	/* The connection is going away under the watchers watching it: drop them
	 * (their dtor runs fpm_ws_unregister_watcher(), so the array empties
	 * through the same path userland's event_free() uses). Snapshot ids for
	 * the same reason every other walk here does. */
	n = ctx->watchers_n;
	/* safe_emalloc, no NULL branch: it aborts on OOM (the same shape
	 * fpm_worker_activate_buffered()'s id snapshot uses), so the loop below
	 * has no null to dereference whatever n is. */
	ids = safe_emalloc(n, sizeof(*ids), 0);
	if (n) {
		memcpy(ids, ctx->watchers, n * sizeof(*ids));
	}
	for (i = 0; i < n; i++) {
		struct fpm_worker_watcher *watcher = zend_hash_index_find_ptr(&fw.watchers, ids[i]);

		if (watcher && watcher->ws == ctx) {
			zend_hash_index_del(&fw.watchers, ids[i]);
		}
	}
	efree(ids);
	/* The bufferevent stays evhttp's -- see the comment in
	 * fpmng_worker_upgrade() -- so tearing the connection down is a
	 * shutdown(), not a bufferevent_free(): the client sees the connection
	 * die, the fd and the SSL* are closed by evhttp_free() at teardown. What
	 * is already queued (a close frame, most often -- the codec writes it
	 * and calls fclose() in the same breath) gets its chance first: with
	 * bytes still unwritten, the callbacks are pointed at
	 * fpm_ws_close_after_write(), which half-closes the moment they are on
	 * the wire -- the last thing this connection ever does. */
	if (!ctx->orphaned) {
		int ws_fd = (int) bufferevent_getfd(ctx->bev);

		if (ws_fd >= 0 && evbuffer_get_length(bufferevent_get_output(ctx->bev)) > 0) {
			bufferevent_setcb(ctx->bev, NULL, fpm_ws_close_after_write_cb, fpm_ws_close_after_write, ctx->bev);
			bufferevent_enable(ctx->bev, EV_WRITE);
		} else {
			/* Disarm BEFORE the shutdown(): the ctx is efree()d below while
			 * the bev stays evhttp's and enabled, and shutdown() makes the
			 * fd readable (EOF pending) -- on the very next loop dispatch
			 * fpm_ws_eventcb() would run with this ctx as its argument, on
			 * freed heap (issue #442: confirmed worker SIGSEGV, respawn).
			 * The buffer is empty here, so disabling EV_WRITE strands
			 * nothing. Covers ws_fd < 0 too: no fd to shut down, the same
			 * armed callbacks on a still-live bev. */
			bufferevent_setcb(ctx->bev, NULL, NULL, NULL, NULL);
			bufferevent_disable(ctx->bev, EV_READ | EV_WRITE);
			if (ws_fd >= 0) {
				shutdown(ws_fd, SHUT_RDWR);
			}
		}
		ctx->eof = true;
	}
	efree(ctx->watchers);
	efree(ctx);
	return 0;
}

/* The tail of a closed upgraded connection: queued bytes out, then the
 * half-close. arg is the bufferevent -- the context is long gone (freed by
 * fpm_ws_close() above), so this reads nothing but the buffer itself. */
static void fpm_ws_close_after_write(struct bufferevent *bev, short what, void *arg)
{
	int ws_fd;

	(void) arg;
	if (!(what & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
		&& evbuffer_get_length(bufferevent_get_output(bev)) > 0) {
		return;	/* still writing; the write callback fires again */
	}
	ws_fd = (int) bufferevent_getfd(bev);
	if (ws_fd >= 0) {
		shutdown(ws_fd, SHUT_RDWR);
	}
}

static void fpm_ws_close_after_write_cb(struct bufferevent *bev, void *arg)
{
	fpm_ws_close_after_write(bev, 0, arg);
}

static int fpm_ws_cast(php_stream *stream, int castas, void **ret)
{
	struct fpm_ws_ctx *ctx = stream->abstract;
	int fd;

	if (ctx->orphaned) {
		/* The fd belongs to the freed bufferevent (#443): nothing to cast. */
		return FAILURE;
	}
	fd = (int) bufferevent_getfd(ctx->bev);

	switch (castas) {
		case PHP_STREAM_AS_FD:
		case PHP_STREAM_AS_FD_FOR_SELECT:
		case PHP_STREAM_AS_SOCKETD:
			if (fd < 0) {
				return FAILURE;
			}
			if (ret) {
				*(int *) ret = fd;
			}
			return SUCCESS;
		default:
			return FAILURE;
	}
}

static int fpm_ws_set_option(php_stream *stream, int option, int value, void *ptrparam)
{
	struct fpm_ws_ctx *ctx = stream->abstract;

	(void) value;
	(void) ptrparam;
	switch (option) {
		case PHP_STREAM_OPTION_CHECK_LIVENESS:
			/* _php_stream_eof() marks the stream EOF when this reports the
			 * stream unlivable -- the answer must come from the bufferevent's
			 * event callback (fpm_ws_eventcb()), never from "the buffer is
			 * empty right now", which on a WebSocket is the NORMAL state. */
			return ctx->eof ? PHP_STREAM_OPTION_RETURN_ERR : PHP_STREAM_OPTION_RETURN_OK;
		default:
			return PHP_STREAM_OPTION_RETURN_NOTIMPL;
	}
}

static const php_stream_ops fpm_ws_ops = {
	fpm_ws_write,	/* write */
	fpm_ws_read,	/* read */
	fpm_ws_close,	/* close */
	NULL,			/* flush: bytes reach the socket when the loop drains the bufferevent */
	"fpmng-ws",		/* label */
	NULL,			/* seek: a stream is not seekable */
	fpm_ws_cast,	/* cast */
	NULL,			/* stat */
	fpm_ws_set_option,	/* set_option */
};

static bool fpm_ws_is_ws_stream(php_stream *stream)
{
	return stream->ops == &fpm_ws_ops;
}

/* Sec-WebSocket-Accept: base64(SHA1(key || GUID)), RFC 6455 section 4.2.1.
 * ext/standard's SHA1 and base64 are PHPAPI and always linked. */
static zend_string *fpm_ws_accept_key(const char *ws_key)
{
	static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	PHP_SHA1_CTX sha;
	unsigned char digest[20];
	size_t key_len = strlen(ws_key);
	char *buf = emalloc(key_len + sizeof(guid));

	/* sized to the actual key: a truncated key||GUID hashes to the wrong
	 * Accept and the compliant client hangs on the handshake */
	memcpy(buf, ws_key, key_len);
	memcpy(buf + key_len, guid, sizeof(guid));
	PHP_SHA1Init(&sha);
	PHP_SHA1Update(&sha, (const unsigned char *) buf, key_len + sizeof(guid) - 1);
	PHP_SHA1Final(digest, &sha);
	efree(buf);
	return php_base64_encode(digest, sizeof(digest));
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_upgrade, 0, 2, IS_RESOURCE, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, response_headers, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* Returns an ordinary bidirectional php_stream over the hijacked connection:
 * fread()/fwrite()/fclose()/feof()/stream_has_buffered() work on it, and so do
 * the watcher primitives, which see the underlying fd. Throws (before any
 * state changed, so the request stays answerable by fpmng_worker_respond())
 * when the id is unknown/answered, the client is gone, or the request is not
 * an RFC 6455 upgrade candidate: a GET with "Upgrade: websocket" and a
 * Sec-WebSocket-Key. */
static ZEND_FUNCTION(fpmng_worker_upgrade)
{
	zend_long id;
	HashTable *response_headers;
	struct fpm_worker_pending *p;
	struct fpm_ws_ctx *ctx;
	struct evhttp_connection *connection;
	struct bufferevent *bev;
	struct evkeyvalq *in;
	const char *upgrade, *key;
	zend_string *accept;
	smart_str head = {0};
	zend_string *name;
	zval *value;
	php_stream *stream;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_LONG(id)
		Z_PARAM_ARRAY_HT(response_headers)
	ZEND_PARSE_PARAMETERS_END();

	p = fpm_worker_pending_get(id);
	if (!p || !p->http) {
		zend_argument_value_error(1, "is not an in-flight request");
		RETURN_THROWS();
	}
	in = evhttp_request_get_input_headers(p->http);
	upgrade = evhttp_find_header(in, "Upgrade");
	key = evhttp_find_header(in, "Sec-WebSocket-Key");
	if (evhttp_request_get_command(p->http) != EVHTTP_REQ_GET || !upgrade
		|| evutil_ascii_strcasecmp(upgrade, "websocket") != 0 || !key || !*key) {
		zend_argument_value_error(1, "is not a WebSocket upgrade request");
		RETURN_THROWS();
	}

	accept = fpm_ws_accept_key(key);

	smart_str_appends(&head, "HTTP/1.1 101 Switching Protocols\r\n");
	smart_str_appends(&head, "Upgrade: websocket\r\n");
	smart_str_appends(&head, "Connection: Upgrade\r\n");
	smart_str_appends(&head, "Sec-WebSocket-Accept: ");
	smart_str_appends(&head, ZSTR_VAL(accept));
	smart_str_appends(&head, "\r\n");
	zend_string_release(accept);

	/* Caller headers (Sec-WebSocket-Protocol and friends), validated the same
	 * way every other response header this executor sends is: the name must
	 * be an RFC 9110 token, the hop-by-hop list still applies (Upgrade and
	 * Connection are this function's to set), and one bad header refuses the
	 * upgrade -- nothing has been written yet, so the request remains
	 * answerable. */
	ZEND_HASH_FOREACH_STR_KEY_VAL(response_headers, name, value) {
		if (!name || !fpm_http_direct_header_name_ok(ZSTR_VAL(name))
			|| fpm_http_direct_header_dropped(ZSTR_VAL(name))) {
			smart_str_free(&head);
			zend_argument_value_error(2, "contains an invalid or hop-by-hop header name");
			RETURN_THROWS();
		}
		if (Z_TYPE_P(value) != IS_STRING) {
			smart_str_free(&head);
			zend_argument_value_error(2, "must be an array of strings");
			RETURN_THROWS();
		}
		{
			/* The value goes into a hand-written response head, bypassing
			 * evhttp_add_header()'s own validation -- a \r\n in it would be
			 * response splitting, and the value is often the client's own
			 * Sec-WebSocket-Protocol echoed back. */
			const char *v = Z_STRVAL_P(value);
			size_t vlen = Z_STRLEN_P(value), vi;

			for (vi = 0; vi < vlen; vi++) {
				if (v[vi] == '\r' || v[vi] == '\n' || v[vi] == '\0') {
					smart_str_free(&head);
					zend_argument_value_error(2, "contains a header value with a control character");
					RETURN_THROWS();
				}
			}
		}
		smart_str_appends(&head, ZSTR_VAL(name));
		smart_str_appends(&head, ": ");
		smart_str_appends(&head, Z_STR_P(value) ? Z_STRVAL_P(value) : "");
		smart_str_appends(&head, "\r\n");
	} ZEND_HASH_FOREACH_END();
	smart_str_appends(&head, "\r\n");

	ctx = emalloc(sizeof(*ctx));
	memset(ctx, 0, sizeof(*ctx));

	/* evhttp owns the connection until the request is answered -- which it
	 * never will be. The hijack follows evws_new_session()'s
	 * evhttp_start_ws_() step for step, on the public 2.1 API -- and where
	 * 2.2's internal version steals evcon->bufev and frees the connection,
	 * this one keeps BOTH alive: the public API can neither reach
	 * evcon->bufev (to null it before a free) nor detach a connection from
	 * its evhttp, and 2.1's own evhttp_connection_free() shutdown(fd,
	 * SHUT_WR)s the connection on the way out, which would kill the
	 * WebSocket the moment it was born. So:
	 *
	 *   - our callbacks replace evhttp's, and its state machine on this
	 *     connection is never driven again;
	 *   - the closecb and the timeouts go (an idle WebSocket must not be cut
	 *     by http.read_timeout; liveness is the codec's ping/pong);
	 *   - the request is left exactly where evhttp put it: never answered,
	 *     never driven, freed with the connection at evhttp_free();
	 *   - the stream's close does NOT free the bufferevent -- it
	 *     shutdown(fd, SHUT_RDWR)s, which is what tears the connection down
	 *     from the client's side -- because evhttp still owns it and frees
	 *     it (fd and SSL) at teardown; a registry of hijacked connections
	 *     (fpm_ws_all, walked in child_main() just before evhttp_free())
	 *     tells a late stream free the bufferevent is gone.
	 *
	 * The cost, honestly stated: one bufferevent plus one request per
	 * WebSocket connection ever accepted stays allocated until the worker
	 * recycles (pm.max_requests, worker.max_lifetime, the master's own
	 * recycling), the same bound every other per-connection allocation of
	 * this executor lives under. */
	connection = evhttp_request_get_connection(p->http);
	bev = connection ? evhttp_connection_get_bufferevent(connection) : NULL;
	if (!bev) {
		/* the client is already gone */
		smart_str_free(&head);
		efree(ctx);
		fpm_worker_reap(p);
		RETURN_FALSE;
	}
	ctx->bev = bev;

	/* Everything that can fail now happens BEFORE any of evhttp's callbacks
	 * are replaced: a failure path below leaves the connection exactly as the
	 * accept built it -- the request is still answerable by
	 * fpmng_worker_respond(), the same contract every throw above keeps. */
	stream = php_stream_alloc(&fpm_ws_ops, ctx, 0, "r+b");
	if (!stream) {
		smart_str_free(&head);
		efree(ctx);
		RETURN_FALSE;
	}
	ctx->stream = stream;
	ctx->next = fpm_ws_all;
	fpm_ws_all = ctx;

	/* The point of no return, one run with no loop iteration inside: evhttp's
	 * callbacks go, ours go in, the 101 is written. */
	bufferevent_setcb(bev, fpm_ws_readcb, fpm_ws_writecb, fpm_ws_eventcb, ctx);
	bufferevent_set_timeouts(bev, NULL, NULL);
	bufferevent_enable(bev, EV_READ | EV_WRITE);
	evhttp_connection_set_closecb(connection, NULL, NULL);
	p->http = NULL;

	bufferevent_write(bev, ZSTR_VAL(head.s), ZSTR_LEN(head.s));
	smart_str_free(&head);

	/* The connection leaves the pending world: worker.max_pending and
	 * worker.request_timeout do not apply to it any more, and it counts
	 * towards pm.max_requests like any other answered request -- the recycle
	 * tail is fpmng_worker_respond_end()'s, without the reply counting (the
	 * 101 never goes through fpm_worker_count_reply()). */
	fpm_worker_reap(p);
	fw.answered++;
	fpm_worker_count_scoreboard_request();
	if (fw.wp->config->pm_max_requests && fw.answered >= (unsigned) fw.wp->config->pm_max_requests &&
		!fpm_worker_stopping) {
		fpm_worker_stopping = 1;
		fpm_http_direct_ops_worker_recycle(fw.ops, FPM_WORKER_RECYCLE_MAX_REQUESTS);
		fpm_worker_notify();
	}

	RETURN_RES(stream->res);
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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_may_exit, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

/* The question a bridge has to answer before it tears its loop down, and the
 * one it cannot answer from its own bookkeeping. An in-flight counter sees
 * only what fpmng_worker_next_request() already handed over; fw.ready holds
 * requests fpm_worker_accept() queued behind it, and the read watcher that
 * would collect them is not polled again until the next loop iteration. The
 * window is one iteration wide and needs no signal at all, because
 * fpmng_worker_respond() is itself what trips pm.max_requests: a request can
 * be queued in the very iteration in which the last in-flight response sets
 * fpm_worker_stopping, and a bridge that concludes "stopping and nothing in
 * flight" then closes it with no response (task 080).
 *
 * fw.pending is the whole truth — every accepted request that has not been
 * answered, queued or handed over — so this is the entire condition. A bridge
 * built on it carries no counter and needs to know nothing about the queue.
 *
 * Deliberate caveat: a handler that never answers keeps this false for ever.
 * That is the pending-table leak fpm_worker_accept() already describes,
 * bounded by the master's stop timeout, and holding the worker open is the
 * better failure — fpm_worker_finish_output() answers whatever is still
 * unanswered on the way out. */
static ZEND_FUNCTION(fpmng_worker_may_exit)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_BOOL(fpm_worker_stopping && zend_hash_num_elements(&fw.pending) == 0);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_stream_has_buffered, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_INFO(0, stream)
ZEND_END_ARG_INFO()

/* Companion to fpm_worker_activate_buffered(), for a driver that wants to make
 * the decision itself rather than be told by an activated watcher: amphp's
 * byte-stream does a direct read before arming a watcher, which is why it never
 * hit the stranding this file otherwise produced, and this makes that trick
 * writable generically instead of by knowing about TLS. Re-casting is a side
 * effect on purpose — for an SSL stream the cast is what moves SSL_pending()
 * bytes into the buffer, so a "false" from this function means the bytes are
 * genuinely not there yet rather than merely not fetched. */
static ZEND_FUNCTION(fpmng_worker_stream_has_buffered)
{
	zval *stream;
	php_stream *php_stream_handle;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_RESOURCE(stream)
	ZEND_PARSE_PARAMETERS_END();

	/* Z_PARAM_RESOURCE accepts any resource, so this is the check that rejects
	 * a curl handle or an already closed stream, and it throws rather than
	 * returning false — returning a value with an exception pending is what
	 * every other builtin in this file avoids. */
	php_stream_from_zval_no_verify(php_stream_handle, stream);
	if (!php_stream_handle) {
		RETURN_THROWS();
	}
	/* issue #343: an upgraded WebSocket connection buffers in its
	 * bufferevent's input evbuffer, not in the php_stream's own read buffer
	 * the generic path below peeks at. */
	if (fpm_ws_is_ws_stream(php_stream_handle)) {
		struct fpm_ws_ctx *ctx = php_stream_handle->abstract;

		if (ctx->orphaned) {
			/* The bufferevent was freed at teardown (#443): nothing can be
			 * buffered any more. */
			RETURN_FALSE;
		}
		RETURN_BOOL(!ctx->eof && evbuffer_get_length(bufferevent_get_input(ctx->bev)) > 0);
	}
	RETURN_BOOL(fpm_worker_stream_buffered(php_stream_handle) > 0);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_next_request, 0, 0, IS_LONG, 1)
ZEND_END_ARG_INFO()

static ZEND_FUNCTION(fpmng_worker_next_request)
{
	zend_ulong id;

	ZEND_PARSE_PARAMETERS_NONE();
	while (fw.ready_count) {
		id = fw.ready[fw.ready_head];
		fw.ready_head = (fw.ready_head + 1) % fw.ready_max;
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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_closed_requests, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* issue #342: drain the ids whose client hung up before the request was
 * answered. The notify pipe tells a driver *that* something happened -- one of
 * the events it carries is a disconnect -- but not *which* request died: the
 * driver used to learn that only from the next fpmng_worker_respond_chunk()
 * returning false, which for a stream that emits an event every 15 s kept a
 * dead client's subscription alive for up to one heartbeat interval per
 * stream. Returned oldest first, ids as ints, and emptied -- the next call
 * returns only closes that happened since, the same drain shape
 * fpmng_worker_next_request() has for its queue. */
static ZEND_FUNCTION(fpmng_worker_closed_requests)
{
	ZEND_PARSE_PARAMETERS_NONE();

	array_init_size(return_value, fw.closed_count);
	while (fw.closed_count) {
		zend_ulong id = fw.closed[fw.closed_head];
		fw.closed_head = (fw.closed_head + 1) % fw.closed_max;
		fw.closed_count--;
		add_next_index_long(return_value, (zend_long) id);
	}
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_request_env, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
ZEND_END_ARG_INFO()

static int fpm_worker_emit_env(void *ctx, const char *key, const char *value)
{
	add_assoc_string((zval *) ctx, key, value);
	return 0;
}

/* CGI-shaped request metadata, built on demand from the live evhttp request by
 * the shared builder — the same variables, the same exclusions and the same
 * refusal of an over-long header name as the classic transport. An empty array
 * means the id is unknown, its client is gone, or the request was refused. */
static ZEND_FUNCTION(fpmng_worker_request_env)
{
	zend_long id;
	struct fpm_worker_pending *p;
	const struct fpm_http_direct_env_source source = {
		.script = fw.script,
		.root = fw.root,
		.front_controller = fw.wp->config->http_front_controller,
		.server_addr = fw.server_addr,
		.server_port = fw.server_port,
		.server_software = "php-fpm-ng/http-direct-worker",
		.tls = fpm_http_direct_tls_enabled(fw.wp),
	};

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(id)
	ZEND_PARSE_PARAMETERS_END();

	p = fpm_worker_pending_get(id);
	if (!p || !p->http) {
		RETURN_EMPTY_ARRAY();
	}
	array_init(return_value);
	if (fpm_http_direct_build_env(p->http, &source, fpm_worker_emit_env, return_value) < 0) {
		zend_array_destroy(Z_ARR_P(return_value));
		RETURN_EMPTY_ARRAY();
	}
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_request_body, 0, 1, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* Drains the request body on the first call and caches it in
 * p->body_cache (issue #335): every later call for the same id returns a
 * fresh reference to that same cached string instead of touching the evbuffer
 * again, which by the first call's own doing has nothing left in it. Before
 * this fix, a second call returned "" — indistinguishable from "the request
 * genuinely had no body" — which made the drain-once behavior a trap for any
 * caller that read the body more than once (e.g. once to check
 * Content-Length against what actually arrived, once to hand it to the
 * handler). http.max_body already bounds the drain in libevent
 * (evhttp_set_max_body_size). */
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
	if (p->body_cache) {
		RETURN_STR_COPY(p->body_cache);
	}
	in = evhttp_request_get_input_buffer(p->http);
	len = evbuffer_get_length(in);
	if (!len) {
		/* Cache the empty string too: a request with no body must keep
		 * answering "" on a second call, the same as one with a body
		 * answers its cached bytes, rather than re-checking an evbuffer
		 * that will never gain anything for this request. */
		p->body_cache = ZSTR_EMPTY_ALLOC();
		RETURN_STR_COPY(p->body_cache);
	}
	zend_string *body = zend_string_alloc(len, 0);
	if (evbuffer_remove(in, ZSTR_VAL(body), len) < 0) {
		zend_string_efree(body);
		RETURN_EMPTY_STRING();
	}
	ZSTR_VAL(body)[len] = '\0';
	p->body_cache = body;
	RETURN_STR_COPY(p->body_cache);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_respond, 0, 4, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, status, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, headers, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, body, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* Returns false when a header could not be emitted, which the caller turns
 * into a 500 rather than a response missing a header the application asked
 * for — the same contract as the classic transport, which refuses the whole
 * response on a rejected header (fpm_http_direct.c:316-320 records why,
 * :834-835 refuses the response). The name check
 * itself is shared with that transport (issue #102): it used to live here
 * only, so the classic path wrote malformed header lines to the wire. */
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
	if (fpm_http_direct_header_dropped(name)) {
		return true;
	}
	if (!fpm_http_direct_header_name_ok(name)) {
		return false;
	}
	str = zval_try_get_string(value);
	if (!str) {
		return false;
	}
	/* Shared with the classic transport (issue #104), which used to charge the
	 * whole raw header() line including headers it went on to drop. */
	ok = fpm_http_direct_header_charge(total, name, ZSTR_LEN(str)) &&
		evhttp_add_header(out, name, ZSTR_VAL(str)) == 0;
	zend_string_release(str);
	return ok;
}

/* issue #333: the ONLY scoreboard fact this executor can report honestly --
 * see the comment on fpm_http_direct_worker_rejects's pm.status_path, ping.*
 * and access.* entries for why nothing else (stage, duration, CPU, peak
 * memory) follows it. Called once per answered request, from both the buffered path
 * (fpmng_worker_respond()) and the streaming completion path
 * (fpmng_worker_respond_end(), issue #332) -- never from
 * fpmng_worker_respond_start() or _chunk(), which do not conclude a request.
 *
 * fpm_scoreboard_update()'s idle/active/lq/lq_len/max_children_reached/slow_rq
 * arguments all carry their FPM_SCOREBOARD_ACTION_INC "no change" value (0 --
 * see fpm_scoreboard_update_commit()), so this touches requests and nothing
 * else: not request_stage, which fpm_request_reading_headers() would also
 * set and which this executor has no per-request concept of (one
 * fpm_request_accepting(false) call for the child's whole life, see
 * fpm_http_direct_worker_child_main()). */
static void fpm_worker_count_scoreboard_request(void)
{
	fpm_scoreboard_update(0, 0, 0, 0, 1, 0, 0, 0, FPM_SCOREBOARD_ACTION_INC, NULL);
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

	if (!fpm_http_direct_status_final(status)) {
		zend_argument_value_error(2, "must be a final HTTP status between 200 and 599");
		RETURN_THROWS();
	}
	if (ZSTR_LEN(body) > FPM_WORKER_BODY_MAX) {
		zend_argument_value_error(4, "must not exceed %d bytes", FPM_WORKER_BODY_MAX);
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
	/* issue #332: a stream already put its status line (and possibly some
	 * chunks) on the wire; a second, buffered response for the same id would
	 * be a second status line. The entry stays pending -- it is not this
	 * function's place to abort someone else's stream. */
	if (p->streaming) {
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
		fpm_worker_send_error(p->http, 500, NULL);
		fpm_worker_reap(p);
		RETURN_FALSE;
	}

	out = evbuffer_new();
	if (!out) {
		RETURN_FALSE;
	}
	if (ZSTR_LEN(body) && !fpm_http_direct_status_bodyless(p->http, (int) status)) {
		evbuffer_add(out, ZSTR_VAL(body), ZSTR_LEN(body));
	}
	/* issue #336: pm.max_requests must add "Connection: close" to the very
	 * reply that trips it, not merely to every one after -- a client pipelining
	 * requests on a kept-alive connection otherwise has no signal that this was
	 * the last answer it will get and sends one more into a connection about to
	 * be torn down. fw.answered/fpm_worker_stopping is evaluated here, before
	 * the header is written, rather than after fpm_worker_send_reply() the way
	 * the bookkeeping below still runs for everything else it does (counting
	 * the reply, notifying the loop) -- moving the whole block up would fire
	 * fpm_worker_notify() before the reply is even queued, which is not
	 * necessary to fix and not worth the churn. */
	if (fpm_worker_stopping || (fw.wp->config->pm_max_requests &&
			fw.answered + 1 >= (unsigned) fw.wp->config->pm_max_requests)) {
		evhttp_add_header(evhttp_request_get_output_headers(p->http), "Connection", "close");
	}
	/* Drop the close callback before handing the request back: libevent owns
	 * and frees it from here on, and a later close on this connection must not
	 * reach a reaped pending entry. */
	evhttp_connection_set_closecb(evhttp_request_get_connection(p->http), NULL, NULL);
	fpm_worker_send_reply(p->http, (int) status, out);
	evbuffer_free(out);
	fpm_worker_reap(p);

	fw.answered++;
	fpm_worker_count_scoreboard_request();
	if (fw.wp->config->pm_max_requests && fw.answered >= (unsigned) fw.wp->config->pm_max_requests &&
		!fpm_worker_stopping) {
		/* Recycling is the master's contract with pm.max_requests; the worker
		 * script sees it as an ordinary stop request and drains. */
		fpm_worker_stopping = 1;
		fpm_http_direct_ops_worker_recycle(fw.ops, FPM_WORKER_RECYCLE_MAX_REQUESTS);
		fpm_worker_notify();
	}
	RETURN_TRUE;
}

/* --- Streaming responses (issue #332) -------------------------------------
 *
 * fpmng_worker_respond() takes one complete body; a handler that wants to push
 * bytes as it produces them (SSE, long-poll, a slow database cursor) has no
 * way to do that without buffering the whole thing in userland first, capped
 * by FPM_WORKER_BODY_MAX anyway. These three builtins are the streaming
 * counterpart, keyed by the same request id fpmng_worker_next_request() hands
 * out, mirroring http.stream's evhttp_send_reply_start()/_chunk()/_end() in
 * fpm_http_direct.c (classic can only stream because it decides to on its own,
 * from http.stream; here the script decides, per request, by calling _start).
 *
 * Unlike classic, this executor never needs to pump the connection's
 * bufferevent by hand: fpmng_worker_loop() already drives the event base
 * between calls into PHP, so libevent's own writer drains whatever
 * evhttp_send_reply_chunk() queues without this file doing anything special.
 * The backpressure problem classic solves with fpm_direct_stream_pump()'s
 * poll()-and-block loop is solved here instead by worker.send_buffer_limit:
 * fpmng_worker_respond_chunk() refuses to queue past it and returns false, so
 * a driver that wants to keep up with a slow client awaits drain (a write
 * watcher, a timer, whatever its own event loop offers) instead of the worker
 * blocking on one connection while every other request it holds waits behind
 * it -- the whole point of this executor's concurrency model. */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_respond_start, 0, 3, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, status, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, headers, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* Puts a status line and headers on the wire and switches the request to
 * chunked framing, without sending a body yet. false means: the id is
 * unknown or already reaped, the client is already gone, the stream was
 * already started (or the id was already answered outright by
 * fpmng_worker_respond()), a header could not be emitted (the request is then
 * answered 500 and reaped, the same contract fpmng_worker_respond() has), the
 * client speaks HTTP/1.0 (no chunked framing exists there), or the status
 * carries no body (HEAD/204/205/304 -- fpm_http_direct_status_bodyless()).
 * A caller-supplied Content-Length, Transfer-Encoding, Connection, etc. is
 * silently dropped rather than refused, same as fpmng_worker_respond() already
 * does for the one-shot path (fpm_http_direct_header_dropped(), called from
 * fpm_worker_add_header() below) -- this transport owns framing either way.
 * None of these throws: like fpmng_worker_respond(), "the client can't have
 * this" is reported through the return value, not an exception. */
static ZEND_FUNCTION(fpmng_worker_respond_start)
{
	zend_long id, status;
	HashTable *headers;
	struct fpm_worker_pending *p;
	zend_string *key;
	zval *value;
	size_t total = 0;
	bool headers_ok = true;

	ZEND_PARSE_PARAMETERS_START(3, 3)
		Z_PARAM_LONG(id)
		Z_PARAM_LONG(status)
		Z_PARAM_ARRAY_HT(headers)
	ZEND_PARSE_PARAMETERS_END();

	if (!fpm_http_direct_status_final(status)) {
		zend_argument_value_error(2, "must be a final HTTP status between 200 and 599");
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
	if (p->streaming) {
		/* Not idempotent: a second status line for the same request is not a
		 * thing evhttp_send_reply_start() can be asked for, so this is
		 * refused rather than silently repeated. */
		RETURN_FALSE;
	}
	/* HTTP/1.0 has no chunked framing: libevent would answer with the output
	 * buffer empty (Content-Length: 0, since nothing has been queued yet) and
	 * then write chunks after it, which a keep-alive HTTP/1.0 client has no
	 * way to tell apart from the start of a second response. Same check
	 * classic's fpm_direct_stream_begin() makes (fpm_http_direct.c). */
	if (p->http->major != 1 || p->http->minor < 1) {
		RETURN_FALSE;
	}
	/* HEAD/204/205/304 carry no body: nothing to stream, and the buffered path
	 * drops the bytes for the same statuses. */
	if (fpm_http_direct_status_bodyless(p->http, (int) status)) {
		RETURN_FALSE;
	}
	ZEND_HASH_FOREACH_STR_KEY_VAL(headers, key, value) {
		if (!key || !fpm_worker_add_header(evhttp_request_get_output_headers(p->http), ZSTR_VAL(key),
				value, &total)) {
			headers_ok = false;
		}
	} ZEND_HASH_FOREACH_END();
	if (!headers_ok) {
		evhttp_connection_set_closecb(evhttp_request_get_connection(p->http), NULL, NULL);
		fpm_worker_send_error(p->http, 500, NULL);
		fpm_worker_reap(p);
		RETURN_FALSE;
	}
	/* Same "Connection: close" the buffered path adds in fpmng_worker_respond():
	 * must be on the wire before the status line goes out, so it belongs here
	 * and not in fpmng_worker_respond_end() -- by the time _end() runs, the
	 * headers this stream is going to send have already been flushed. */
	if (fpm_worker_stopping) {
		evhttp_add_header(evhttp_request_get_output_headers(p->http), "Connection", "close");
	}
	/* libevent picks chunked itself for an HTTP/1.1 client with no
	 * Content-Length, exactly as it does from fpm_direct_stream_begin(); no
	 * caller-supplied Content-Length reaches here to disagree with it, see the
	 * function comment. */
	evhttp_send_reply_start(p->http, (int) status, NULL);
	p->streaming = true;
	RETURN_TRUE;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_respond_chunk, 0, 2, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* Queues one chunk on the connection's own bufferevent -- fpmng_worker_loop()
 * writes it out on a later iteration, the same as any other reply this file
 * hands to libevent. false means: the id is unknown, was never started, or is
 * already finished; the client is already gone (discovered here, which reaps
 * the entry so the caller does not have to notice on its own); or queuing this
 * much more would push the connection's already-queued output past
 * worker.send_buffer_limit -- the backpressure lever, refusing to buffer
 * without bound what fpmng_worker_respond()'s FPM_WORKER_BODY_MAX already
 * bounds for a single-body reply. An empty string is accepted and queues
 * nothing: evhttp_send_reply_chunk() with an empty buffer would write the
 * terminating zero-length chunk, ending the stream early, so this file never
 * calls it with one. */
static ZEND_FUNCTION(fpmng_worker_respond_chunk)
{
	zend_long id;
	zend_string *data;
	struct fpm_worker_pending *p;
	struct bufferevent *bev;
	struct evbuffer *out;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_LONG(id)
		Z_PARAM_STR(data)
	ZEND_PARSE_PARAMETERS_END();

	p = fpm_worker_pending_get(id);
	if (!p || !p->streaming) {
		RETURN_FALSE;
	}
	if (!p->http) {
		fpm_worker_reap(p);
		RETURN_FALSE;
	}
	if (fw.wp->config->worker_send_buffer_limit > 0 && ZSTR_LEN(data)) {
		bev = evhttp_connection_get_bufferevent(evhttp_request_get_connection(p->http));
		if (bev && evbuffer_get_length(bufferevent_get_output(bev)) + ZSTR_LEN(data) >
				fw.wp->config->worker_send_buffer_limit) {
			RETURN_FALSE;
		}
	}
	if (ZSTR_LEN(data)) {
		out = evbuffer_new();
		if (!out) {
			RETURN_FALSE;
		}
		/* A fresh evbuffer per call, freed right after it drains into the
		 * connection: the same convention fpmng_worker_respond() uses for its
		 * one-shot body, not a buffer kept across calls. */
		evbuffer_add(out, ZSTR_VAL(data), ZSTR_LEN(data));
		evhttp_send_reply_chunk(p->http, out);
		evbuffer_free(out);
	}
	RETURN_TRUE;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_worker_respond_end, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* Sends the terminating chunk and does the same bookkeeping
 * fpmng_worker_respond()'s tail does: drop the close callback, count the
 * reply so fpm_worker_finish_output() waits for it to reach the wire, reap the
 * pending entry, and apply pm.max_requests recycling. false means the id is
 * unknown, was never started, was already ended, or the client is already
 * gone (reaped here, same as fpmng_worker_respond_chunk()). */
static ZEND_FUNCTION(fpmng_worker_respond_end)
{
	zend_long id;
	struct fpm_worker_pending *p;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(id)
	ZEND_PARSE_PARAMETERS_END();

	p = fpm_worker_pending_get(id);
	if (!p || !p->streaming) {
		RETURN_FALSE;
	}
	if (!p->http) {
		fpm_worker_reap(p);
		RETURN_FALSE;
	}
	/* No "Connection: close" of our own here: for a stream the headers are
	 * long gone by _end() time (evhttp_send_reply_start() already flushed
	 * them from fpmng_worker_respond_start()), which is where that header, if
	 * any, was already added. */
	/* Drop the close callback before handing the request back: libevent owns
	 * and frees it from here on, and a later close on this connection must not
	 * reach a reaped pending entry. fpm_worker_count_reply() installs its own
	 * closecb/on_complete_cb right after, same as fpm_worker_send_reply()'s
	 * callers do for the one-shot path. */
	evhttp_connection_set_closecb(evhttp_request_get_connection(p->http), NULL, NULL);
	fpm_worker_count_reply(p->http);
	evhttp_send_reply_end(p->http);
	fpm_worker_reap(p);

	fw.answered++;
	fpm_worker_count_scoreboard_request();
	if (fw.wp->config->pm_max_requests && fw.answered >= (unsigned) fw.wp->config->pm_max_requests &&
		!fpm_worker_stopping) {
		/* Recycling is the master's contract with pm.max_requests; the worker
		 * script sees it as an ordinary stop request and drains. */
		fpm_worker_stopping = 1;
		fpm_http_direct_ops_worker_recycle(fw.ops, FPM_WORKER_RECYCLE_MAX_REQUESTS);
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
	php_stream *php_stream_handle = NULL;
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
			/* Userland works on PHP streams, libevent on descriptors. A
			 * stream with no usable descriptor cannot be watched at all; one
			 * that buffers in userland (filters, TLS) can, because
			 * fpm_worker_activate_buffered() re-casts it every iteration. */
			zend_argument_type_error(2, "must be a stream with a usable file descriptor");
			RETURN_THROWS();
		}
	}
	watcher = pemalloc(sizeof(*watcher), 1);
	watcher->id = fw.next_id++;
	watcher->spin = 0;
	watcher->buffered = 0;
	watcher->spin_warned = false;
	watcher->type = (short) type;
	watcher->ws = NULL;
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
	/* issue #343: an upgraded WebSocket connection's data lands in its
	 * bufferevent, not in the descriptor -- bind the watcher to the
	 * connection so the bufferevent callbacks can fire it. Timers carry no
	 * stream, and php_stream_handle is untouched for them. */
	if (type != FPM_WORKER_EV_TIMER && fpm_ws_is_ws_stream(php_stream_handle)) {
		struct fpm_ws_ctx *ctx = php_stream_handle->abstract;

		watcher->ws = ctx;
		if (ctx->watchers_n == ctx->watchers_cap) {
			ctx->watchers_cap = ctx->watchers_cap ? ctx->watchers_cap * 2 : 4;
			ctx->watchers = erealloc(ctx->watchers, ctx->watchers_cap * sizeof(*ctx->watchers));
		}
		ctx->watchers[ctx->watchers_n++] = watcher->id;
	}
	zend_hash_index_add_new_ptr(&fw.watchers, watcher->id, watcher);
	/* issue #333: see the matching call in fpm_worker_reap(). */
	fpm_worker_metrics_publish(fw.metrics, zend_hash_num_elements(&fw.pending), zend_hash_num_elements(&fw.watchers));
	fpm_worker_ops_publish_live();
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
	if (!watcher) {
		RETURN_FALSE;
	}
	/* fpm_worker_activate_buffered() stops sampling a disabled watcher, so the
	 * baseline it kept is stale by the time userland enables it again. */
	watcher->spin = 0;
	watcher->buffered = 0;
	watcher->spin_warned = false;
	RETURN_BOOL(event_del(watcher->ev) == 0);
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
	/* issue #333: see the matching call in fpm_worker_reap(). */
	fpm_worker_metrics_publish(fw.metrics, zend_hash_num_elements(&fw.pending), zend_hash_num_elements(&fw.watchers));
	fpm_worker_ops_publish_live();
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
	struct timeval now;
	double stall_seconds;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_BOOL(blocking)
	ZEND_PARSE_PARAMETERS_END();

	if (fw.running) {
		zend_throw_error(NULL, "fpmng_worker_loop(): the event loop is already running; "
			"libevent allows only one event_base_loop() per base at a time");
		RETURN_THROWS();
	}
	/* issue #339: fpmng_pool_worker_loop_iterations_total /
	 * fpmng_pool_worker_loop_stall_seconds_max. Measured on entry, not exit --
	 * this call itself can block for as long as the caller's `blocking`
	 * argument and libevent's own timers allow, and it is exactly that gap
	 * between one entry and the next this metric exists to catch: a userland
	 * driver that got stuck somewhere else and stopped calling back in here at
	 * all. fw.loop_last_entry's zero value is the "no previous call yet"
	 * sentinel -- see its field comment. */
	gettimeofday(&now, NULL);
	if (fw.loop_last_entry.tv_sec || fw.loop_last_entry.tv_usec) {
		stall_seconds = (double) (now.tv_sec - fw.loop_last_entry.tv_sec) +
			(double) (now.tv_usec - fw.loop_last_entry.tv_usec) / 1000000.0;
	} else {
		stall_seconds = -1;
	}
	fw.loop_last_entry = now;
	fpm_http_direct_ops_worker_loop_iteration(fw.ops, stall_seconds);
	/* Before libevent, never after: it is what keeps a stream whose bytes sit
	 * in a userland buffer from parking this iteration on an idle descriptor.
	 * Inside fw.running, because its refill can call a user-space stream's
	 * stream_cast() method, and that userland must hit the guard above rather
	 * than reach a nested event_base_loop() on this base. */
	fw.running = true;
	/* issue #338: a listener this worker disabled comes back here if its cooldown
	 * has passed, before libevent is given the chance to sleep -- so a worker
	 * that keeps looping does not need the timer wakeup to reopen. */
	fpm_worker_accept_reopen();
	fpm_worker_activate_buffered();
	if (EG(exception)) {
		fw.running = false;
		RETURN_THROWS();
	}
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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpm_connection_info, 0, 0, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, id, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

/* issue #335: fpm_connection_info() on pool.executor = worker, keyed by the
 * same request id fpmng_worker_next_request() hands out -- unlike the classic
 * executor, this one runs several requests concurrently against one PHP
 * engine, so there is no single ambient "current connection"
 * (fpm_direct_current in fpm_http_direct.c) for a zero-argument call to mean
 * anything: the entire point of the id parameter is "which of several
 * in-flight requests do you mean", and there is no sensible default for that.
 * `id = 0` (the omitted-argument default) therefore answers `false`
 * unconditionally, the same "nothing to report" convention every other
 * builtin in this file uses, rather than picking an arbitrary in-flight
 * request to report on. An unknown or already-answered id -- fpm_worker_pending_get()
 * returns NULL, or p->http is NULL because the client is already gone --
 * answers `false` the same way.
 *
 * age/requests are omitted, not present as false/null/0: this executor never
 * runs fpm_http_direct_conn.c's periodic tick (track_live) to keep a
 * per-connection node alive between requests -- doing that per connection
 * here would leak one fd's worth of bookkeeping for the life of the worker,
 * the very leak that tracking exists to avoid on the classic executor. There
 * is therefore no accept time and no served-request count this executor can
 * honestly report for the connection an id's request arrived on, and it says
 * so by leaving the keys out rather than inventing a value -- the same
 * "report honestly, omit what genuinely cannot be known" convention issue
 * #333's fpm_worker_count_scoreboard_request() documents for the scoreboard. */
static ZEND_FUNCTION(fpm_connection_info)
{
	zend_long id = 0;
	struct fpm_worker_pending *p;
	struct evhttp_connection *conn;
	char *address = NULL;
	ev_uint16_t port = 0;
#ifdef HAVE_FPM_HTTP_TLS
	SSL *ssl = NULL;
	struct bufferevent *bev;
#endif

	ZEND_PARSE_PARAMETERS_START(0, 1)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(id)
	ZEND_PARSE_PARAMETERS_END();

	if (id == 0) {
		RETURN_FALSE;
	}
	p = fpm_worker_pending_get(id);
	if (!p || !p->http) {
		RETURN_FALSE;
	}

	array_init(return_value);

	conn = evhttp_request_get_connection(p->http);
	if (conn) {
		evhttp_connection_get_peer(conn, &address, &port);
	}
	add_assoc_string(return_value, "peer_addr", address ? address : "");
	add_assoc_long(return_value, "peer_port", port);
	/* No "age"/"requests" keys -- see the function comment above. */

#ifdef HAVE_FPM_HTTP_TLS
	bev = conn ? evhttp_connection_get_bufferevent(conn) : NULL;
	if (bev) {
		ssl = bufferevent_openssl_get_ssl(bev);
	}
	if (ssl) {
		const unsigned char *alpn = NULL;
		unsigned int alpn_len = 0;
		const char *sni;
		X509 *cert;

		add_assoc_string(return_value, "transport", "tls");
		add_assoc_string(return_value, "tls_protocol", (char *) SSL_get_version(ssl));
		add_assoc_string(return_value, "tls_cipher", (char *) SSL_get_cipher_name(ssl));

		SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
		if (alpn && alpn_len > 0) {
			add_assoc_stringl(return_value, "tls_alpn", (char *) alpn, alpn_len);
		} else {
			add_assoc_null(return_value, "tls_alpn");
		}

		sni = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
		if (sni) {
			add_assoc_string(return_value, "tls_sni", sni);
		} else {
			add_assoc_null(return_value, "tls_sni");
		}

		/* Client certificate fields: same "meaningless without it" gate
		 * classic's fpm_connection_info() uses -- present only when this
		 * pool's http.tls_verify_client asked the client for one at all. */
		if (SSL_get_verify_mode(ssl) & SSL_VERIFY_PEER) {
			cert = SSL_get1_peer_certificate(ssl);
			if (cert) {
				char *subject = fpm_http_direct_x509_name(X509_get_subject_name(cert));
				char *issuer = fpm_http_direct_x509_name(X509_get_issuer_name(cert));
				char *not_before = fpm_http_direct_x509_time(X509_get0_notBefore(cert));
				char *not_after = fpm_http_direct_x509_time(X509_get0_notAfter(cert));
				char *fingerprint = fpm_http_direct_x509_fingerprint(cert);

				add_assoc_bool(return_value, "client_cert_verified",
					SSL_get_verify_result(ssl) == X509_V_OK);
				if (subject) { add_assoc_string(return_value, "client_cert_subject", subject); efree(subject); }
				else { add_assoc_null(return_value, "client_cert_subject"); }
				if (issuer) { add_assoc_string(return_value, "client_cert_issuer", issuer); efree(issuer); }
				else { add_assoc_null(return_value, "client_cert_issuer"); }
				if (not_before) { add_assoc_string(return_value, "client_cert_not_before", not_before); efree(not_before); }
				else { add_assoc_null(return_value, "client_cert_not_before"); }
				if (not_after) { add_assoc_string(return_value, "client_cert_not_after", not_after); efree(not_after); }
				else { add_assoc_null(return_value, "client_cert_not_after"); }
				if (fingerprint) { add_assoc_string(return_value, "client_cert_fingerprint_sha256", fingerprint); efree(fingerprint); }
				else { add_assoc_null(return_value, "client_cert_fingerprint_sha256"); }

				X509_free(cert);
			} else {
				add_assoc_bool(return_value, "client_cert_verified", 0);
				add_assoc_null(return_value, "client_cert_subject");
				add_assoc_null(return_value, "client_cert_issuer");
				add_assoc_null(return_value, "client_cert_not_before");
				add_assoc_null(return_value, "client_cert_not_after");
				add_assoc_null(return_value, "client_cert_fingerprint_sha256");
			}
		}
	} else {
		add_assoc_string(return_value, "transport", "plain");
	}
#else
	add_assoc_string(return_value, "transport", "plain");
#endif
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpm_send_early_hints, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, headers, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, id, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

/* issue #335: fpm_send_early_hints() on pool.executor = worker. Same id
 * parameter and same no-id/unknown-id -> false rationale as
 * fpm_connection_info() just above -- there is no ambient "current
 * connection" to default to when several requests are in flight against one
 * PHP engine.
 *
 * "Already responded" guard: p->streaming is true from fpmng_worker_respond_start()
 * until fpmng_worker_respond_end() reaps the entry,
 * so it is exactly "a status line may already be on the wire" for a request
 * that is still in fw.pending. A request answered outright by the buffered
 * fpmng_worker_respond() needs no flag of its own: fpm_worker_reap() removes
 * it from fw.pending on that path, so fpm_worker_pending_get() already
 * returns NULL for it, caught by the `!p` check below. Together these two
 * checks are the worker-executor equivalent of classic's
 * `r->responded || r->streaming || SG(headers_sent)`.
 *
 * Header validation, serialization and the wire write mirror
 * fpm_http_direct.c's fpm_send_early_hints() exactly: the same
 * fpm_http_direct_header_dropped()/fpm_http_direct_header_name_ok()/
 * fpm_http_direct_header_charge()/evhttp_add_header() validation chain
 * fpm_worker_add_header() already reuses for the final response, the same
 * "HTTP/1.1 103 Early Hints\r\n" + headers + "\r\n" byte layout, written to
 * the connection's bufferevent with bufferevent_write_buffer() rather than
 * through evhttp's request/reply state -- evhttp has no interim-response API
 * (see the classic function's comment for why). */
static ZEND_FUNCTION(fpm_send_early_hints)
{
	HashTable *headers;
	zend_long id = 0;
	struct fpm_worker_pending *p;
	struct evhttp_connection *conn;
	struct bufferevent *bev;
	zend_string *key;
	zval *value;
	struct evkeyvalq validated = {0};
	struct evkeyval *kv;
	struct evbuffer *out;
	size_t total = 0;

	ZEND_PARSE_PARAMETERS_START(1, 2)
		Z_PARAM_ARRAY_HT(headers)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(id)
	ZEND_PARSE_PARAMETERS_END();

	if (id == 0) {
		RETURN_FALSE;
	}
	p = fpm_worker_pending_get(id);
	if (!p || !p->http) {
		RETURN_FALSE;
	}
	if (p->streaming) {
		RETURN_FALSE;
	}
	if (p->http->major != 1 || p->http->minor < 1) {
		RETURN_FALSE;
	}

	validated.tqh_last = &validated.tqh_first;
	ZEND_HASH_FOREACH_STR_KEY_VAL(headers, key, value) {
		if (!key) {
			continue;
		}
		fpm_worker_add_header(&validated, ZSTR_VAL(key), value, &total);
	} ZEND_HASH_FOREACH_END();

	out = evbuffer_new();
	if (!out) {
		evhttp_clear_headers(&validated);
		RETURN_FALSE;
	}
	evbuffer_add_printf(out, "HTTP/1.1 103 Early Hints\r\n");
	for (kv = validated.tqh_first; kv; kv = kv->next.tqe_next) {
		evbuffer_add_printf(out, "%s: %s\r\n", kv->key, kv->value);
	}
	evbuffer_add_printf(out, "\r\n");
	evhttp_clear_headers(&validated);

	conn = evhttp_request_get_connection(p->http);
	bev = conn ? evhttp_connection_get_bufferevent(conn) : NULL;
	if (!bev || bufferevent_write_buffer(bev, out) < 0) {
		evbuffer_free(out);
		RETURN_FALSE;
	}
	evbuffer_free(out);
	RETURN_TRUE;
}

static const zend_function_entry fpm_worker_functions[] = {
	ZEND_FE(fpmng_worker_notify_stream, arginfo_fpmng_worker_notify_stream)
	ZEND_FE(fpmng_worker_stopping, arginfo_fpmng_worker_stopping)
	ZEND_FE(fpmng_worker_may_exit, arginfo_fpmng_worker_may_exit)
	ZEND_FE(fpmng_worker_stream_has_buffered, arginfo_fpmng_worker_stream_has_buffered)
	ZEND_FE(fpmng_worker_next_request, arginfo_fpmng_worker_next_request)
	ZEND_FE(fpmng_worker_closed_requests, arginfo_fpmng_worker_closed_requests)
	ZEND_FE(fpmng_worker_upgrade, arginfo_fpmng_worker_upgrade)
	ZEND_FE(fpmng_worker_request_env, arginfo_fpmng_worker_request_env)
	ZEND_FE(fpmng_worker_request_body, arginfo_fpmng_worker_request_body)
	ZEND_FE(fpmng_worker_respond, arginfo_fpmng_worker_respond)
	ZEND_FE(fpmng_worker_respond_start, arginfo_fpmng_worker_respond_start)
	ZEND_FE(fpmng_worker_respond_chunk, arginfo_fpmng_worker_respond_chunk)
	ZEND_FE(fpmng_worker_respond_end, arginfo_fpmng_worker_respond_end)
	ZEND_FE(fpmng_worker_event_create, arginfo_fpmng_worker_event_create)
	ZEND_FE(fpmng_worker_event_enable, arginfo_fpmng_worker_event_enable)
	ZEND_FE(fpmng_worker_event_disable, arginfo_fpmng_worker_event_disable)
	ZEND_FE(fpmng_worker_event_free, arginfo_fpmng_worker_event_free)
	ZEND_FE(fpmng_worker_loop, arginfo_fpmng_worker_loop)
	ZEND_FE(fpmng_worker_loop_break, arginfo_fpmng_worker_loop_break)
	ZEND_FE(fpm_connection_info, arginfo_fpm_connection_info)
	ZEND_FE(fpm_send_early_hints, arginfo_fpm_send_early_hints)
	ZEND_FE_END
};

/* task 076: zend_register_functions() unconditionally does
 * `internal_function->module = EG(current_module)` (Zend/zend_API.c:2987),
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
 * (zend_optimizer.c:113-118):
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
	/* Issue #60, with this executor's twist: one php_request_startup() covers
	 * the whole worker, so the hook fires once and the .user.ini next to the
	 * worker script governs every request the worker then serves from its own
	 * loop. That is the only meaning the file can have here -- there is no
	 * per-request ini stage to revert to -- and docs/http-direct.md says so. */
	sapi_module.pre_request_init = fpm_http_direct_user_ini_pre_request;
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
	struct fpm_http_direct_conns_limits limits = {0};
	struct sigaction action = {0}, term_before;
	struct sockaddr_storage address;
	socklen_t address_len = sizeof(address);
	zend_file_handle file;

	fw.wp = wp;
	fw.next_id = 1;
	/* Before anything is accepted, and fatal on a malformed address: a list
	 * meant to keep someone out must never end up keeping nobody out. */
	if (wp->config->listen_allowed_clients && *wp->config->listen_allowed_clients &&
		fpm_http_acl_parse(wp->config->name, "listen.allowed_clients",
			wp->config->listen_allowed_clients, &fw.acl) < 0) {
		exit(FPM_EXIT_CONFIG);
	}
	/* Against the directory the child actually chdir'd into, not against the
	 * configured one the master already checked. */
	if (fpm_http_direct_resolve_script(NULL, wp->config->http_front_controller, fw.root, fw.script) < 0) {
		zlog(ZLOG_ERROR, "[pool %s] %s: %s must be a regular file inside chdir", wp->config->name,
			fpm_worker_labels.script_context, fpm_worker_labels.script_noun);
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
	/* Before the bevcb is installed, because the first connection this child
	 * accepts already goes through it. Both limits are zero by design (this
	 * executor rejects them) and so is limits.track_live: only the deadline is
	 * tracked here, no connection is kept past its first request, and that is
	 * why this executor needs no sweep and therefore no periodic tick. Asking
	 * for track_live without a sweep would leak the fd of every connection
	 * that sends a request -- see fpm_http_direct_conn.h. */
	limits.pool = wp->config->name;
	limits.read_timeout_ms = wp->config->http_read_timeout;
	fw.conns = fpm_http_direct_conns_new(fw.base, &limits);
	if (!fw.conns) {
		exit(FPM_EXIT_SOFTWARE);
	}
	evhttp_set_max_headers_size(fw.http, FPM_HTTP_HEADERS_MAX);
	evhttp_set_max_body_size(fw.http, wp->config->http_max_body);
	evhttp_set_timeout_tv(fw.http, &timeout);
	evhttp_set_allowed_methods(fw.http, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD |
		EVHTTP_REQ_PUT | EVHTTP_REQ_DELETE | EVHTTP_REQ_OPTIONS | EVHTTP_REQ_PATCH);
	evhttp_set_gencb(fw.http, fpm_worker_accept, NULL);
	/* See the same call in fpm_http_direct.c: before the listener, so no
	 * connection is ever accepted in the plain. The on-accept hook here arms the
	 * first-request deadline of issue #61 and counts the accept for the ceiling
	 * of issue #338 -- which is not the accept gate of issue #53, see "THE
	 * ACCEPT CEILING" above. */
	if (fpm_http_direct_tls_child_attach(wp, fw.base, fw.http, fpm_worker_accept_hook, NULL) < 0) {
		exit(FPM_EXIT_SOFTWARE);
	}
	/* On a TLS pool the hook above is already installed on the TLS bevcb,
	 * which fpm_http_direct_tls.c reinstalls on every certificate reload. */
	if (!fpm_http_direct_tls_enabled(wp)) {
		evhttp_set_bevcb(fw.http, fpm_worker_bevcb, NULL);
	}
	fw.listener = evhttp_accept_socket_with_handle(fw.http, wp->listening_socket);
	if (!fw.listener) {
		exit(FPM_EXIT_SOFTWARE);
	}
	zend_hash_init(&fw.pending, 16, NULL, fpm_worker_pending_dtor, 1);
	zend_hash_init(&fw.watchers, 16, NULL, fpm_worker_watcher_dtor, 1);
	/* issue #333: claims this child's pending/watcher gauge slot. Best-effort --
	 * fw.metrics may come back NULL (no scoreboard proc, or the pool's
	 * init_main() never allocated the segment) and every publish call below is
	 * a no-op on NULL, so a worker still serves requests without these two
	 * gauges rather than failing to start over a metrics channel. */
	fw.metrics = fpm_http_direct_worker_metrics_init_child(wp);
	/* issue #339: claims this child's slot in fpm_http_direct_ops's shared
	 * table -- the same one the classic executor uses for pm.status_path,
	 * now also allocated for this executor by
	 * fpm_pool_type_http_direct_worker_init(). Same best-effort/NULL-safe
	 * contract as fw.metrics above. */
	fw.ops = fpm_http_direct_ops_init_child(wp);
	/* issue #331: worker.max_pending, validated > 0 in
	 * fpm_http_direct_worker_validate(). Sized once, here, rather than at
	 * every fpm_worker_accept() -- the config never changes for the life of
	 * this child. */
	fw.ready_max = (unsigned) wp->config->worker_max_pending;
	fw.ready = pemalloc(fw.ready_max * sizeof(*fw.ready), 1);
	/* issue #342: the closed-id ring, one slot per pending entry at most --
	 * see the field comment on fw.closed. */
	fw.closed_max = (unsigned) wp->config->worker_max_pending;
	fw.closed = pemalloc(fw.closed_max * sizeof(*fw.closed), 1);
	/* issue #338: worker.accept_threshold, 0 = off. fpm_conf_set_integer()
	 * already refuses a negative value, so there is nothing to clamp; read
	 * once here for the same reason ready_max is. */
	fw.accept_threshold = (unsigned) wp->config->worker_accept_threshold;
	if (fw.accept_threshold) {
		/* One-shot, armed by fpm_worker_accept_maybe_close() and never pending
		 * while the gate is open, so a pool that never reaches its ceiling pays
		 * no wakeups for it. */
		fw.accept_cooldown = evtimer_new(fw.base, fpm_worker_accept_tick, NULL);
		if (!fw.accept_cooldown) {
			zlog(ZLOG_ERROR, "[pool %s] http-direct worker: failed to create the "
				"worker.accept_threshold cooldown timer", wp->config->name);
			exit(FPM_EXIT_SOFTWARE);
		}
	}
	/* issue #331: worker.request_timeout, 0 = off. No timer at all in that
	 * case -- see the field comment on fw.request_timeout_sweep for why this
	 * is one periodic sweep rather than one timer per pending request. The
	 * sweep interval is a quarter of the timeout (floored, so a very short
	 * timeout still gets checked often enough to matter) rather than the
	 * timeout itself, trading a bit of extra wakeups for a worst-case overshoot
	 * of one sweep interval instead of one whole timeout period. */
	if (wp->config->worker_request_timeout > 0) {
		int sweep_ms = wp->config->worker_request_timeout / FPM_WORKER_REQUEST_TIMEOUT_SWEEP_DIVISOR;
		struct timeval sweep_tv;

		if (sweep_ms < FPM_WORKER_REQUEST_TIMEOUT_SWEEP_FLOOR_MS) {
			sweep_ms = FPM_WORKER_REQUEST_TIMEOUT_SWEEP_FLOOR_MS;
		}
		sweep_tv.tv_sec = sweep_ms / 1000;
		sweep_tv.tv_usec = (sweep_ms % 1000) * 1000;
		fw.request_timeout_sweep = event_new(fw.base, -1, EV_PERSIST, fpm_worker_sweep_expired, NULL);
		if (!fw.request_timeout_sweep || event_add(fw.request_timeout_sweep, &sweep_tv) < 0) {
			zlog(ZLOG_ERROR, "[pool %s] http-direct worker: failed to arm the worker.request_timeout sweep",
				wp->config->name);
			exit(FPM_EXIT_SOFTWARE);
		}
	}
	/* issue #334: worker.max_memory/worker.max_lifetime, 0/0 = off. Armed
	 * whenever EITHER is non-zero, unlike worker.request_timeout_sweep above:
	 * that timer's own interval is derived from the one directive it serves,
	 * so it is simply absent when that directive is 0 and cannot double as
	 * this check's clock. fw.start_time is stamped here too, right before the
	 * timer that reads it back. */
	fw.start_time = time(NULL);
	if (wp->config->worker_max_memory > 0 || wp->config->worker_max_lifetime > 0) {
		struct timeval health_tv = {FPM_WORKER_HEALTH_SWEEP_INTERVAL_SEC, 0};

		fw.health_sweep = event_new(fw.base, -1, EV_PERSIST, fpm_worker_health_sweep, NULL);
		if (!fw.health_sweep || event_add(fw.health_sweep, &health_tv) < 0) {
			zlog(ZLOG_ERROR, "[pool %s] http-direct worker: failed to arm the worker.max_memory/"
				"worker.max_lifetime health sweep", wp->config->name);
			exit(FPM_EXIT_SOFTWARE);
		}
	}
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
	/* SA_RESTART, as upstream's fpm_signals_init_child() does: the booted
	 * script is in userland whenever it is not inside the loop, so a signal
	 * that arrives mid-syscall would otherwise surface to it as an EINTR I/O
	 * error instead of a retry. */
	action.sa_flags = SA_RESTART;
	if (sigaction(SIGQUIT, &action, NULL) < 0) {
		exit(FPM_EXIT_SOFTWARE);
	}
	/* issue #65 retires one child with SIGUSR1. Here it is the same handler as
	 * SIGQUIT, and deliberately so: this executor tracks no connections (no
	 * limits, no gauge -- see limits.track_live at the bottom of this file), so
	 * it has nothing with which to tell "drain the connections I hold" from
	 * "drain the requests I hold". What the shared handler does buy is that the
	 * signal an operator sends to retire a child of any direct pool is never
	 * the SIG_DFL that would kill this one outright. */
	if (sigaction(SIGUSR1, &action, NULL) < 0) {
		exit(FPM_EXIT_SOFTWARE);
	}
	fpm_worker_install_sapi();
	/* Issue #60: the directory of the worker script this child actually
	 * resolved, never the configured string and never anything a client sends. */
	if (fpm_http_direct_user_ini_init_child(wp->config->name, fw.root, fw.script) < 0) {
		exit(FPM_EXIT_CONFIG);
	}
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
	/* Issue #259: request startup also took SA_RESTART off the SIGQUIT and
	 * SIGUSR1 handlers installed above -- zend_signal_activate() reinstalls
	 * every signal in zend_sigs[] with SA_SIGINFO alone. This executor is the
	 * one that needs it most: the booted script keeps running between loop
	 * turns, so a retire signal can land in the middle of any syscall it makes. */
	fpm_http_direct_restore_sa_restart(SIGQUIT);
	fpm_http_direct_restore_sa_restart(SIGUSR1);
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
	EG(exit_status) = 0;
	zend_first_try {
		/* Inside the try, not above it, for the reason fpm_pool_script.c gives
		 * at the same point: these four calls are the first code here that
		 * runs PHP, so they are the first that can bail out, and
		 * php_request_startup() has already cleared EG(bailout) by the time it
		 * returns. Outside a try, zend_bailout() -- an E_ERROR from a stream
		 * wrapper, memory_limit exhausted while opening php://stdout -- takes
		 * "Bailed out without a bailout address!" and exit(-1), skipping
		 * php_request_shutdown(), fpm_stdio_flush_child() and
		 * event_base_free(): the master sees an unexplained exit code and
		 * respawns straight back into the same condition. Inside it, the
		 * failure lands in zend_end_try() and the child takes the ordinary
		 * "the worker script returned" path, which names the cause.
		 *
		 * Issue #73 for the three stream constants. Both example bridges
		 * report a failed handler with fwrite(STDERR, ...) from inside their
		 * catch, and until this call existed that threw "Undefined constant"
		 * from the one path whose job is to swallow the failure: in the amphp
		 * bridge the Error escaped the fiber into Revolt's uncaught-throwable
		 * handler, so one failing request killed the worker instead of logging
		 * a line. Registered once and not per request because one
		 * php_request_startup() covers the whole worker
		 * (fpm_worker_ub_write), which is also what makes the three dup()s a
		 * one-off rather than the per-run leak fpm_std_streams_register()
		 * describes. Same route as echo: ub_write already writes to
		 * STDERR_FILENO, and the master collects it under
		 * catch_workers_output. */
		fpm_std_streams_register(wp->config->name);
		/* NOT CONST_PERSISTENT (issue #149). That flag is what the optimizer
		 * folds on -- Zend/Optimizer/pass1.c folds both a ZEND_FETCH_CONSTANT
		 * and defined() for a constant carrying it -- and these three exist
		 * in worker children only, while the opcache SHM entry is keyed on
		 * the script path and shared by every pool of one master. A bootstrap
		 * included by a worker pool and by a classic/fiber/fcgi pool would
		 * otherwise get whichever process compiled it first baked in: a value
		 * for a constant that does not exist in the reader. Without the flag
		 * each process resolves them at runtime, one hash lookup per fetch.
		 * Covered by sapi/fpmng/tests/fpmng-http-direct-worker-opcache.phpt. */
		REGISTER_MAIN_LONG_CONSTANT("FPMNG_WORKER_READ", FPM_WORKER_EV_READ, 0);
		REGISTER_MAIN_LONG_CONSTANT("FPMNG_WORKER_WRITE", FPM_WORKER_EV_WRITE, 0);
		REGISTER_MAIN_LONG_CONSTANT("FPMNG_WORKER_TIMER", FPM_WORKER_EV_TIMER, 0);

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
		/* issue #339: fpmng_pool_worker_recycles_total{reason="script_returned"}.
		 * Every other recycle reason sets fpm_worker_stopping itself before the
		 * script gets a chance to return on its own; reaching this branch means
		 * none of them did. */
		fpm_http_direct_ops_worker_recycle(fw.ops, FPM_WORKER_RECYCLE_SCRIPT_RETURNED);
	}
	/* issue #333: a graceful exit zeroes this child's gauges immediately
	 * rather than leaving the last published value for a successor child to
	 * overwrite whenever it claims this slot -- the same staleness window
	 * fpm_http_direct_worker_live_gauges() documents as acceptable for an
	 * UNgraceful exit (killed, no chance to run this line), but avoidable
	 * here. */
	fpm_worker_metrics_publish(fw.metrics, 0, 0);
	/* issue #339: same reasoning, for this executor's own gauges. */
	fpm_http_direct_ops_worker_publish(fw.ops, &(struct fpm_http_direct_ops_worker_live) {0});
	/* Before php_request_shutdown(), not after: a watcher holds a zval
	 * callback allocated by this request, so releasing it once the request
	 * arena is gone would be a use-after-free. Freeing the events here also
	 * guarantees none outlives event_base_free() below. */
	zend_hash_destroy(&fw.watchers);
	/* After the watchers, never before: fpm_worker_finish_output() drives
	 * the base itself, and a userland watcher still registered there would
	 * call into PHP after the worker script has already returned. */
	fw.finishing = true;
	fpm_worker_finish_output();
	fw.finishing = false;
	/* After finish_output(), which may still drive the base and therefore let
	 * this fire once more; before event_base_free() below, which the event
	 * must be freed ahead of. Harmless either way since fw.pending is still
	 * alive here, but nothing is left for it to check once every remaining
	 * entry is about to be destroyed unanswered. */
	if (fw.request_timeout_sweep) {
		event_free(fw.request_timeout_sweep);
		fw.request_timeout_sweep = NULL;
	}
	/* issue #334: same reasoning as request_timeout_sweep just above. */
	if (fw.health_sweep) {
		event_free(fw.health_sweep);
		fw.health_sweep = NULL;
	}
	/* issue #338: likewise. The listener this timer would have reopened is
	 * already gone by now (evhttp_del_accept_socket() in fpm_worker_stop()), so
	 * there is nothing left for a late tick to do. */
	if (fw.accept_cooldown) {
		event_free(fw.accept_cooldown);
		fw.accept_cooldown = NULL;
		fw.accept_cooling = false;
	}
	/* Strictly before zend_hash_destroy(&fw.pending). evhttp_free() closes the
	 * still-open server connections and *does* fire their close callbacks;
	 * measured against libevent 2.1: "before evhttp_free, got_closecb=0" /
	 * "CLOSECB fired" / "after evhttp_free, got_closecb=1". Each of those
	 * callbacks writes p->http = NULL through a struct fpm_worker_pending, so
	 * destroying the table first turns every abandoned request into a heap
	 * write into freed memory. Reachable whenever the worker script stops with
	 * requests still in flight — an escaping exception, exit(), or a handler
	 * that never answered. */
	/* Before evhttp_free() and event_base_free(): a tracked connection holds an
	 * event on this base and a reference to a bufferevent evhttp is about to
	 * drop. */
	fpm_http_direct_conns_free(fw.conns);
	/* issue #343: the bufferevents of hijacked WebSocket connections stay
	 * evhttp's for the child's whole life (see fpmng_worker_upgrade()) -- the
	 * free below frees them, and a stream shell php_request_shutdown() frees
	 * later must not touch its (now gone) bufferevent again. */
	{
		struct fpm_ws_ctx *ctx;

		for (ctx = fpm_ws_all; ctx; ctx = ctx->next) {
			ctx->orphaned = true;
			/* feof() / stream_select() liveness checks run during the
			 * shutdown-function window below and must answer "the peer is
			 * gone", not "alive" (#443). The stream shell still exists --
			 * php_request_shutdown() frees it later. */
			ctx->eof = true;
			if (ctx->stream) {
				ctx->stream->eof = 1;
			}
		}
	}
	evhttp_free(fw.http);
	zend_hash_destroy(&fw.pending);
	/* issue #331: the ring buffer allocated in the setup above, next to the
	 * table it indexes into -- freed here for the same reason, not before. */
	pefree(fw.ready, 1);
	/* issue #342: the closed-id ring, allocated next to fw.ready above. */
	pefree(fw.closed, 1);
	fpm_http_direct_worker_metrics_free(fw.metrics);
	fw.metrics = NULL;
	fpm_http_direct_ops_free(fw.ops);
	fw.ops = NULL;
	php_request_shutdown(NULL);
	sigaction(SIGTERM, &term_before, NULL);
	SG(server_context) = NULL;
	fpm_stdio_flush_child();
	event_base_free(fw.base);
	exit(FPM_EXIT_OK);
}
