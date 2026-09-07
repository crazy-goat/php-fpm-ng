#ifndef FPM_HTTP_H
#define FPM_HTTP_H 1

struct fpm_worker_pool_s;

/* HTTP gateways for one pool (see fpm_http.c). Called by the "http" pool type
 * from fpm_pool_type.c, on the master side, before worker fork. */
int fpm_http_init_pool(struct fpm_worker_pool_s *wp);

/* As above, but for an executor handling multiple requests per worker.
 * capacity = number of concurrent FastCGI connections for the whole pool. */
int fpm_http_init_pool_with_capacity(struct fpm_worker_pool_s *wp, unsigned capacity);

/* Validate http.* directives for pool.type = http, called from the pool type's
 * .validate hook in fpm_pool_type.c while checking configuration, before any fork. */
int fpm_http_validate_pool(struct fpm_worker_pool_s *wp);

#endif
