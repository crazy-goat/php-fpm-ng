#ifndef FPM_HTTP_DIRECT_H
#define FPM_HTTP_DIRECT_H 1

struct fpm_worker_pool_s;

extern const char *const fpm_http_direct_rejects[];
int fpm_http_direct_validate(struct fpm_worker_pool_s *wp);
void fpm_http_direct_child_main(struct fpm_worker_pool_s *wp);

#endif
