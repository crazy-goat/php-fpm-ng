#ifndef FPM_HTTP_H
#define FPM_HTTP_H 1

struct fpm_worker_pool_s;

/* Bramki HTTP dla jednego poola (patrz fpm_http.c). Wolane przez typ poola
 * "http" z fpm_pool_type.c, ze strony mastera, przed forkiem workerow. */
int fpm_http_init_pool(struct fpm_worker_pool_s *wp);

#endif
