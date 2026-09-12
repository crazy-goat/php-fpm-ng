/* fpm-ng: pool.type = status — a pool that does not run PHP at all. It listens
 * for HTTP on its own port (the same mechanism as listen for fcgi/http, but
 * directly, without fcgi+1 as the HTTP gateway), reads the scoreboards and
 * shared memory of ALL other pools in the same master, and serializes them in
 * two formats: Prometheus text (/metrics) and JSON (/status). See
 * docs/NOTES.md section 3u for the design rationale.
 */

#ifndef FPM_POOL_STATUS_H
#define FPM_POOL_STATUS_H 1

struct fpm_worker_pool_s;

/* Directives rejected for pool.type = status. NULL-terminated, used as
 * .rejects in fpm_pool_types[]. Unlike supervisor/cron, "listen"/"listen."
 * are NOT rejected here — status really does listen. */
extern const char *const fpm_pool_status_rejects[];

/* fpm_pool_type_s.validate — status has no type-specific directives to check;
 * the only job is to enforce pm = static + 1 programmatically (one process is
 * entirely sufficient for monitoring scrapes, so no "number of processes"
 * directive is needed). */
int fpm_pool_status_validate(struct fpm_worker_pool_s *wp);

/* fpm_pool_type_s.child_main — the shared operator HTTP server
 * (fpm_operator_http.h) on this pool's own socket, answering Prometheus text on
 * /metrics and JSON on /status, 404 everything else. Does not return. */
void fpm_pool_status_child_main(struct fpm_worker_pool_s *wp);

/* Render every pool (only = NULL) or exactly one (only = that pool) into an
 * operator HTTP response body. Exported for the per-pool operator endpoint
 * (fpm_operator_endpoint.c, issue #274), which serves the same two formats from
 * a different listener and must not grow its own copy of them.
 *
 * Called from the endpoint's own child, so like everything else here they read
 * only shared memory and configuration -- never another process's heap. */
struct fpm_operator_buf_s;
void fpm_pool_status_render_prometheus(struct fpm_operator_buf_s *b, struct fpm_worker_pool_s *only);
void fpm_pool_status_render_json(struct fpm_operator_buf_s *b, struct fpm_worker_pool_s *only);

#endif
