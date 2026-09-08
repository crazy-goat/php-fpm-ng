/* fpm-ng: worker-mode HTTP-direct (POC, task 073). See fpm_http_direct_worker.c. */

#ifndef FPM_HTTP_DIRECT_WORKER_H
#define FPM_HTTP_DIRECT_WORKER_H 1

struct fpm_worker_pool_s;

extern const char *const fpm_http_direct_worker_rejects[];

int fpm_http_direct_worker_validate(struct fpm_worker_pool_s *wp);
void fpm_http_direct_worker_child_main(struct fpm_worker_pool_s *wp);

#endif
