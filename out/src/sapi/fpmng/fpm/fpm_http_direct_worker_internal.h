/* fpm-ng: worker-mode HTTP-direct, state shared between
 * fpm_http_direct_worker.c and fpm_http_direct_ws.c. Private to those two
 * files; the struct definitions and the worker state moved out of
 * fpm_http_direct_worker.c verbatim when the WebSocket code (issue #343) got a
 * file of its own (#747), and the comments are theirs. */
#ifndef FPM_HTTP_DIRECT_WORKER_INTERNAL_H
#define FPM_HTTP_DIRECT_WORKER_INTERNAL_H 1

#include "fpm_config.h"

#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <sys/time.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include "php.h"
#include "php_streams.h"

#define FPM_WORKER_EV_READ 1
#define FPM_WORKER_EV_WRITE 2
#define FPM_WORKER_EV_TIMER 3

struct fpm_worker_pending {
	struct evhttp_request *http; /* NULL after the connection died */
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
	/* issue #459: a nonempty chunk was refused by worker.send_buffer_limit.
	 * This is retryable flow control, not client-gone: the request remains live
	 * while the worker is healthy. During retirement it is the one state that
	 * may_exit() hands to fpm_worker_finish_output() to terminate cleanly. */
	bool backpressured;
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

/* The hijacked WebSocket connection of fpmng_worker_upgrade(); the long design
 * comment is above its code in fpm_http_direct_ws.c. */
struct fpm_ws_ctx {
	struct bufferevent *bev;
	struct evhttp_connection *evcon; /* the connection bev belongs to (#472) */
	php_stream *stream;
	/* The pending request is reaped at the hijack and evhttp frees its request
	 * before a retained stream closes. Keep a copy of the identity needed to
	 * name that request when userland unwinds before the 101 can flush (#461). */
	zend_ulong request_id;
	char *request_uri;
	bool eof; /* the event callback saw BEV_EVENT_EOF or BEV_EVENT_ERROR */
	bool close_after_write; /* the close tail is counted in fw.ws_unflushed */
	bool failure_logged; /* the failed-upgrade warning was emitted */
	bool orphaned; /* teardown: the bufferevent was (or is being) freed by evhttp_free() */
	/* watcher ids bound to this connection, fired by the bufferevent
	 * callbacks; ids only -- the table re-lookup and the ->ws check happen at
	 * fire time, the same shape fpm_worker_activate_buffered() uses. */
	zend_ulong *watchers;
	unsigned watchers_n;
	unsigned watchers_cap;
	struct fpm_ws_ctx *next;
};

struct fpm_worker_state {
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
	HashTable pending; /* id -> struct fpm_worker_pending * */
	HashTable watchers; /* id -> struct fpm_worker_watcher * */
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
	/* issue #461/#458: upgraded WebSocket connections whose queued output or
	 * nonblocking TLS close_notify still has to reach the socket. Kept separate
	 * from unflushed above: the latter counts ordinary HTTP replies and feeds
	 * the abandoned-request metric, while these are already hijacked requests
	 * whose handshake or close frame is on the wire path. */
	unsigned ws_unflushed;
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
	 * the same one the classic executor uses for operator.status_path's ?full rows
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
};

extern struct fpm_worker_state fw;

/* fpm_http_direct_ws.c */
extern struct fpm_ws_ctx *fpm_ws_all;
void fpm_ws_unregister_watcher(struct fpm_worker_watcher *watcher);
void fpm_ws_prepare_shutdown(bool log_failure);
void fpm_ws_tls_close_abort_all(void);
bool fpm_ws_is_ws_stream(php_stream *stream);
ZEND_FUNCTION(fpmng_worker_upgrade);

/* fpm_http_direct_worker.c */
struct fpm_worker_pending *fpm_worker_pending_get(zend_long id);
void fpm_worker_count_scoreboard_request(void);
void fpm_worker_account_answered_request(void);
void fpm_worker_send_error(struct evhttp_request *http, int status, const char *reason);
void fpm_worker_send_reply(struct evhttp_request *http, int status, struct evbuffer *body);
void fpm_worker_reap(struct fpm_worker_pending *p);
extern volatile sig_atomic_t fpm_worker_stopping;

#endif
