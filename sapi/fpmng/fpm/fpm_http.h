#ifndef FPM_HTTP_H
#define FPM_HTTP_H 1

struct fpm_worker_pool_s;
struct fpm_operator_buf_s;

/* HTTP gateways for one pool (see fpm_http.c). Called by the "http" pool type
 * from fpm_pool_type.c, on the master side, before worker fork. */
int fpm_http_init_pool(struct fpm_worker_pool_s *wp);

/* As above, but for an executor handling multiple requests per worker.
 * capacity = number of concurrent FastCGI connections for the whole pool. */
int fpm_http_init_pool_with_capacity(struct fpm_worker_pool_s *wp, unsigned capacity);

/* Validate http.* directives for pool.type = http, called from the pool type's
 * .validate hook in fpm_pool_type.c while checking configuration, before any fork. */
int fpm_http_validate_pool(struct fpm_worker_pool_s *wp);

/* Issue #341: fpm_pool_type_s.render_metrics_prometheus for pool.type = http --
 * fpmng_gateway_{upstreams_used,upstreams_max,requests_total,rejected_total}
 * per target, on wp's own pm.metrics_path. Called from the operator endpoint's
 * own child (see fpm_pool_type_s's comment on render_metrics_prometheus): the
 * counters it reads are shared memory, allocated once in the master before any
 * child -- including the operator endpoint's -- forks. A wp with no gateway of
 * its own (not pool.type = http, or the gateway failed to start) renders
 * nothing. */
void fpm_http_render_metrics_prometheus(struct fpm_worker_pool_s *wp, struct fpm_operator_buf_s *b);

#endif
