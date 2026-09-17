/* fpm-ng: worker-mode HTTP-direct (POC, task 073). See fpm_http_direct_worker.c. */

#ifndef FPM_HTTP_DIRECT_WORKER_H
#define FPM_HTTP_DIRECT_WORKER_H 1

struct fpm_worker_pool_s;

/* Compile-time default for worker.max_pending (issue #331), used by
 * fpm_conf.c to seed wp->config->worker_max_pending before the pool's own
 * config is parsed. Bounds the memory a client burst can pin in
 * accepted-but-unanswered requests; beyond it the transport answers 503
 * itself, as the gateway does when its worker budget is exhausted
 * (fpm_http.c:153-155). Runtime checks in fpm_http_direct_worker.c use the
 * configured value, never this macro directly. */
#define FPM_WORKER_PENDING_MAX 256

extern const char *const fpm_http_direct_worker_rejects[];

int fpm_http_direct_worker_validate(struct fpm_worker_pool_s *wp);
void fpm_http_direct_worker_child_main(struct fpm_worker_pool_s *wp);

#endif
