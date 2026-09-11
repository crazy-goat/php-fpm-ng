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

/* Counts a request this child refused: listen.allowed_clients, or no capacity
 * left. The caller sends the response; this only accounts for it. */
void fpm_http_direct_ops_refused(struct fpm_http_direct_ops *ops);

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
