/* fpm-ng: the operator surface of pool.type = http-direct (issue #59) --
 * listen.allowed_clients, ping.path and pm.status_path.
 *
 * These three were refused by the type until now, and each was refused for the
 * same reason: a direct pool answers the client from the child's own event
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

/* 1 when the peer may be served, 0 when listen.allowed_clients excludes it.
 * peer may be NULL (a connection whose address libevent could not report),
 * which is treated as not allowed whenever a list is configured. */
int fpm_http_direct_ops_allowed(struct fpm_http_direct_ops *ops, const char *peer);

/* ping.path / pm.status_path. Answers and returns 1 when the request is one of
 * them, 0 when it is not and the caller should carry on. `status` and `bytes`
 * receive what was sent, for the access log. */
int fpm_http_direct_ops_try_local(struct fpm_http_direct_ops *ops, struct evhttp_request *http,
	int *status, size_t *bytes);

#endif
