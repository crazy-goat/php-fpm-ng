/* fpm-ng: pool.executor = async — EXPERIMENT.
 *
 * One process, multiple FastCGI requests in flight, each in its own True Async
 * coroutine (fork php-src true-async/php-src + ext/async). The implementation
 * is currently a POC and validate() rejects the pool on every engine because
 * the request container does not yet have the required isolation or guards.
 * Rationale and limitations: docs/async_errors.md and section 3t of
 * docs/NOTES.md.
 */

#ifndef FPM_POOL_ASYNC_H
#define FPM_POOL_ASYNC_H 1

struct fpm_worker_pool_s;

/* Directives rejected for pool.executor = async (see fpm_pool_type_check_directives). */
extern const char *const fpm_pool_async_rejects[];

/* fpm_pool_type_s.validate — currently rejects the pool until Async has the
 * same guards and isolation as fiber. */
int fpm_pool_async_validate(struct fpm_worker_pool_s *wp);

/* fpm_pool_type_s.child_main — event loop instead of a blocking accept loop. Does not return. */
void fpm_pool_async_child_main(struct fpm_worker_pool_s *wp);

#endif
