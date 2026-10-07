#ifndef FPM_HTTP_DIRECT_H
#define FPM_HTTP_DIRECT_H 1

struct fpm_worker_pool_s;

extern const char *const fpm_http_direct_rejects[];
int fpm_http_direct_validate(struct fpm_worker_pool_s *wp);
void fpm_http_direct_child_main(struct fpm_worker_pool_s *wp);
/* Issue #259. Shared with the worker executor, which installs the same two
 * SA_RESTART handlers and then calls php_request_startup() just as the classic
 * child does; the rationale is at the definition in fpm_http_direct.c. */
void fpm_http_direct_restore_sa_restart(int signo);

#endif
