/* fpm-ng: the operator surface of pool.type = http-direct (issue #59) --
 * listen.allowed_clients, ping.path and operator.status_path.
 *
 * These three were refused by the type until issue #59, and each was refused for
 * the same reason: a direct pool answers the client from the child's own event
 * loop, so none of the code that implements them for a fastcgi pool is on the
 * path. listen.allowed_clients is enforced by fastcgi.c at accept time,
 * ping/status by fpm_main.c between fcgi_accept_request() and the script. A
 * direct child reaches neither.
 *
 * They live together in one file because they are one thing from the outside:
 * what this pool answers before -- and instead of -- the application. The
 * fourth piece of issue #59, access.log, is separate only because it has to
 * render a format (fpm_http_direct_access_log.h).
 *
 * The counters are in shared memory, one slot per child, because a status page
 * must describe the POOL and any child can be the one that answers. Each slot
 * has exactly one writer (its own child) and many readers, and every field is
 * a counter that only grows or a small gauge, so a reader can miss an update
 * but cannot read a torn value on any platform this runs on. A lock would buy
 * consistency between fields that nothing needs: the page is a snapshot of a
 * moving system either way.
 *
 * Issue #64 added the per-connection fields and a schema version on the page.
 * Reset semantics, which the version is there to let tooling reason about:
 * TOTALS belong to the pool and survive a child recycled by pm.max_requests --
 * the successor inherits the slot and keeps counting, and the two totals kept
 * in the child's own memory are published as a difference for that reason --
 * while GAUGES belong to the child and start at zero, because a child killed
 * mid-request is exactly what leaks a gauge. The page ignores the gauges of a
 * slot whose child is gone (the scoreboard's `used` flag), so a pool that
 * scaled down does not keep reporting connections nobody holds. A reload
 * re-executes the master and therefore starts every number over -- measured on
 * the poligon 2026-09-11, accepted conn 9 before and 1 after.
 */

#ifndef FPM_HTTP_DIRECT_OPS_H
#define FPM_HTTP_DIRECT_OPS_H 1

struct evhttp_request;
struct fpm_worker_pool_s;
struct fpm_http_direct_ops;

/* Master, before the first fork: allocates the shared counters for the pool.
 * Returns 0 also when the pool declares none of these directives -- the
 * counters feed the status page, and whether anyone asks for it is not known
 * until a request arrives. */
int fpm_http_direct_ops_init_main(struct fpm_worker_pool_s *wp);

/* Child, before its event loop. Parses listen.allowed_clients (a bad address
 * is fatal, exactly as it is for a fastcgi pool) and finds this child's
 * counter slot. Returns NULL on failure, having said why. */
struct fpm_http_direct_ops *fpm_http_direct_ops_init_child(struct fpm_worker_pool_s *wp);

void fpm_http_direct_ops_free(struct fpm_http_direct_ops *ops);

/* From the accept hook. Counts the connection; the ACL is not checked here
 * because libevent's per-connection hook runs before the peer address is
 * known. */
void fpm_http_direct_ops_accepted(struct fpm_http_direct_ops *ops);

/* Requests in flight on this child, for the "active requests" row. Call with
 * +1 when a request starts being answered and -1 when it is done. */
void fpm_http_direct_ops_active(struct fpm_http_direct_ops *ops, int delta);

/* Counts a request this child answered without running PHP. */
void fpm_http_direct_ops_local(struct fpm_http_direct_ops *ops);

/* Counts a request this child refused. The caller sends the response; this only
 * accounts for it. The reason is kept apart because the three mean different
 * things to whoever is looking: an ACL refusal is a configuration meeting a
 * client, a capacity refusal is this child being full, and a connection
 * refusal is issue #61's per-client cap. A status page that only said
 * "refused" would make an operator guess which. */
enum fpm_http_direct_refusal {
	FPM_HTTP_DIRECT_REFUSED_ACL = 0,	/* listen.allowed_clients */
	FPM_HTTP_DIRECT_REFUSED_CAPACITY,	/* the pending cap, or the pool is stopping */
	FPM_HTTP_DIRECT_REFUSED_MAX
};
void fpm_http_direct_ops_refused(struct fpm_http_direct_ops *ops, enum fpm_http_direct_refusal why);

/* Counts a response this child could not send as the application built it --
 * the body or the header budget was exceeded, or a header name was not a
 * token -- and turned into a 500. Separate from a refusal: the request was
 * accepted and ran, and what failed is the answer (issue #64). */
void fpm_http_direct_ops_rejected(struct fpm_http_direct_ops *ops);

/* The gauges and the connection totals this child owns but does not itself
 * keep: fpm_http_direct_conn.c counts connections, the worker knows its own
 * queue depth, and this is where both become visible to a status reader. Call
 * from the worker's tick -- it is a handful of stores into shared memory, and
 * the numbers are a snapshot either way. */
struct fpm_http_direct_ops_live {
	unsigned connections;		/* held right now by this child */
	/* Responses accepted and not yet fully written. NOT a queue of requests
	 * waiting to execute: the classic executor runs PHP inside evhttp's
	 * request callback, so a request is either executing or done and there is
	 * nothing in between to queue. Both gauges here are published from the
	 * worker's tick, which means they are frozen for as long as that child is
	 * inside PHP -- the loop that would publish them is the loop the script is
	 * blocking. "active requests" is the one that keeps moving there, because
	 * it is written on the request path itself. */
	unsigned pending;
	/* Both are totals since this child started, not deltas: the publish works
	 * out the difference itself, so a caller that simply reports what it has
	 * counted cannot double-count by being called twice. */
	unsigned long timed_out;	/* first request never arrived */
	unsigned long refused_conn;	/* http.max_connections_per_client */
	/* issue #65: 1 while this child is draining towards its own exit. A gauge
	 * like the two above it -- a replacement child in the same slot publishes
	 * 0 and the page stops saying it. */
	unsigned retiring;
};
void fpm_http_direct_ops_publish(struct fpm_http_direct_ops *ops,
	const struct fpm_http_direct_ops_live *live);

/* pool.executor = worker only (issue #339). The plumbing gap the issue starts
 * with: this table already existed for the classic executor, one slot per
 * scoreboard index, and is simply not written to by a worker child before
 * this. Everything below is additive to the classic fields above -- a classic
 * slot never calls any of it and stays zero, and a worker slot never touches
 * conn_live/requests_active/responses_pending above (this executor has no
 * per-request scoreboard stage to publish there, see
 * fpm_http_direct_worker_rejects). Reusing the one table rather than a second
 * one is deliberate: fpm_http_direct_ops_init_child() already resolves "this
 * child's slot" once, from the scoreboard index, and a second lookup would be
 * a second place that decision could disagree with the first. */

/* fpmng_pool_worker_refused_total{pool,reason}, reason as an index rather than
 * a name: what the classic executor's FPM_HTTP_DIRECT_REFUSED_ACL/CAPACITY
 * enum already does, for the same reason -- ACL and "bad request" mean
 * different things to whoever is reading a dashboard and a status page that
 * only said "refused" would make them guess which. "saturated" and "stopping"
 * are the two ways worker.max_pending's ceiling in fpm_worker_accept() answers
 * 503: the first while the worker is otherwise healthy, the second once it has
 * already asked to stop and is refusing everything on the way out -- the same
 * distinction the recycle reasons below draw for why a worker is recycling in
 * the first place. */
enum fpm_http_direct_worker_refused_reason {
	FPM_WORKER_REFUSED_SATURATED = 0,	/* worker.max_pending reached, still healthy */
	FPM_WORKER_REFUSED_STOPPING,		/* worker.max_pending reached while draining */
	FPM_WORKER_REFUSED_ACL,			/* listen.allowed_clients */
	FPM_WORKER_REFUSED_BAD_REQUEST,		/* fpm_http_direct_request_acceptable() said no */
	FPM_WORKER_REFUSED_MAX
};
void fpm_http_direct_ops_worker_refused(struct fpm_http_direct_ops *ops,
	enum fpm_http_direct_worker_refused_reason why);

/* fpmng_pool_worker_recycles_total{pool,reason}: every path that sets
 * fpm_worker_stopping = 1 (fpm_http_direct_worker.c), plus the one path that
 * stops the worker WITHOUT ever setting it -- the script simply returning on
 * its own, counted as "script_returned" at the one place child_main() already
 * distinguishes that case from every asked-for stop below it. Same shape as
 * the supervisor's restarts counter (issue #277): a number that only ever
 * climbing on a healthy pool is the signal, not the count of any one reason by
 * itself. */
enum fpm_http_direct_worker_recycle_reason {
	FPM_WORKER_RECYCLE_MAX_REQUESTS = 0,	/* pm.max_requests */
	FPM_WORKER_RECYCLE_SATURATION,		/* worker.max_pending's ceiling asked the script to drain */
	FPM_WORKER_RECYCLE_MAX_MEMORY,		/* worker.max_memory */
	FPM_WORKER_RECYCLE_MAX_LIFETIME,	/* worker.max_lifetime */
	FPM_WORKER_RECYCLE_SCRIPT_RETURNED,	/* the worker script ended on its own, unasked */
	FPM_WORKER_RECYCLE_SIGNAL,		/* SIGQUIT or SIGUSR1 (issue #65 retire) */
	FPM_WORKER_RECYCLE_MAX
};
void fpm_http_direct_ops_worker_recycle(struct fpm_http_direct_ops *ops,
	enum fpm_http_direct_worker_recycle_reason why);

/* fpmng_pool_worker_abandoned_total{pool}: requests fpm_worker_finish_output()
 * 503'd at shutdown drain (n = however many in one call) or a reply still
 * unwritten when its flush budget ran out (n = 1, one call per event since
 * fw.unflushed has no per-request identity left by then). Both are the same
 * thing to an operator: an accepted request this pool never delivered an
 * answer for. */
void fpm_http_direct_ops_worker_abandoned(struct fpm_http_direct_ops *ops, unsigned n);

/* fpmng_pool_worker_client_gone_total{pool}: the client's connection closed
 * while a request on it was still unanswered (fpm_worker_conn_closed()) --
 * invisible in any status code today, since nothing was ever sent. */
void fpm_http_direct_ops_worker_client_gone(struct fpm_http_direct_ops *ops);

/* fpmng_pool_worker_loop_iterations_total{pool,slot} and
 * fpmng_pool_worker_loop_stall_seconds_max{pool,slot}: called once per
 * fpmng_worker_loop() call, from the ZEND_FUNCTION itself. stall_seconds is
 * the gap since the previous call, or a negative number for the very first
 * call this child ever makes (nothing to compare against yet) -- the counter
 * still increments either way, only the gauge's running max is left alone.
 * Head-of-line blocking (one synchronous call freezing every connection this
 * child holds) is this executor's characteristic failure mode, and the
 * longest gap between loop entries is what catches it directly: a handler
 * that never yields back to fpmng_worker_loop() is a gap that keeps growing
 * until it returns. */
void fpm_http_direct_ops_worker_loop_iteration(struct fpm_http_direct_ops *ops, double stall_seconds);

/* The rest of the worker's gauges, all published from the same mutation
 * points fpm_worker_metrics_publish() already is (fpm_worker_accept(),
 * fpm_worker_reap(), fpmng_worker_event_create()/_free()) -- one child, one
 * set of "how busy am I right now" numbers, so there is no reason for this
 * table to be refreshed on a different schedule than fw.pending/fw.watchers'
 * combined total already is. */
struct fpm_http_direct_ops_worker_live {
	unsigned long queued;			/* fw.ready_count: accepted, not yet handed to PHP */
	/* Seconds, not the raw timeval: a gauge is one double either way, and
	 * every other duration on this page already ends in the same suffix. 0
	 * when nothing is pending -- indistinguishable from "just this instant",
	 * which is the same approximation worker.request_timeout's own sweep
	 * makes when it reads a fresh accepted_at. */
	double pending_oldest_seconds;
	unsigned long watchers_read;
	unsigned long watchers_write;
	unsigned long watchers_timer;
	unsigned long memory_bytes;		/* getrusage(RUSAGE_SELF).ru_maxrss, issue #334's own sample reused */
	unsigned long unflushed;		/* fw.unflushed: replies queued with libevent, not yet on the wire */
};
void fpm_http_direct_ops_worker_publish(struct fpm_http_direct_ops *ops,
	const struct fpm_http_direct_ops_worker_live *live);

/* 1 when the peer may be served, 0 when listen.allowed_clients excludes it.
 * peer may be NULL (a connection whose address libevent could not report),
 * which is treated as not allowed whenever a list is configured. */
int fpm_http_direct_ops_allowed(struct fpm_http_direct_ops *ops, const char *peer);

/* ping.path. Answers and returns 1 when the request is it, 0 when it is not and
 * the caller should carry on. `status` and `bytes` receive what was sent, for
 * the access log.
 *
 * operator.status_path used to be answered here too. Issue #275 moved it onto the
 * operator endpoint's listener (fpm_pool_type_s.operator_status ->
 * fpm_http_direct_ops_render_status below), so that the directive names one
 * page on one socket. ping.path stayed: it is a liveness probe for whatever is
 * in front of the pool, so the public listener is where it belongs (#273,
 * point 9). */
int fpm_http_direct_ops_try_local(struct fpm_http_direct_ops *ops, struct evhttp_request *http,
	int *status, size_t *bytes);

/* fpm_pool_type_s.operator_status for this type: the page above, rendered for
 * `wp` into an operator HTTP response body. `query` is the request's query
 * string without the '?', empty when there was none; "json" and "full" are the
 * two flags it understands, exactly as they were on the old listener.
 *
 * Called in the operator endpoint's child, which is not one of this pool's
 * children, so everything it reads is a shared segment: the per-slot counters
 * the master allocated in fpm_http_direct_ops_init_main() and wp->scoreboard.
 * The same foreign read the generic operator pages do. */
struct fpm_operator_reply_s;
void fpm_http_direct_ops_render_status(struct fpm_worker_pool_s *wp, const char *query,
	struct fpm_operator_reply_s *reply);

/* fpm_pool_type_s.render_metrics_prometheus for the worker executor (issue
 * #339): the twelve series above, appended to the operator endpoint's
 * Prometheus page after the generic per-pool lines and after live_gauges'
 * fpmng_pool_worker_pending/fpmng_pool_worker_watchers (issue #333). Same
 * shape as fpm_http_direct_ops_render_status above -- a type-specific
 * callback that owns writing its own lines into the buffer -- rather than
 * live_gauges' fixed few-scalars array: a slot label multiplies every gauge
 * by pm.max_children, which is exactly the "more than a handful" case
 * FPM_POOL_LIVE_GAUGES_MAX's own comment says does not belong in that array.
 *
 * Called in the operator endpoint's own child, same foreign-shared-memory
 * read as fpm_http_direct_ops_render_status. */
struct fpm_operator_buf_s;
void fpm_http_direct_ops_render_worker_metrics_prometheus(struct fpm_worker_pool_s *wp,
	struct fpm_operator_buf_s *b);

#endif
