/* fpm-ng: pool.executor = fiber — the interception registry (issue #531).
 *
 * One static table (fpm_pool_fiber_intercept.c) lists every IO interception
 * in install order. The scheduler walks it instead of calling each module's
 * install function by hand, and fiber.disable_interceptions switches single
 * entries off. Adding one (curl) is one file plus one table line; removing one
 * is the reverse, and nothing else refers to it. */

#ifndef FPM_POOL_FIBER_INTERCEPT_H
#define FPM_POOL_FIBER_INTERCEPT_H 1

struct fpm_worker_pool_s;

/* Master side, from the fiber executor's validate(): every name in
 * fiber.disable_interceptions must be an entry of the table. 0 = ok,
 * -1 = ALERT logged. */
int fpm_fiber_intercept_validate(struct fpm_worker_pool_s *wp);

/* Child side, once, after every extension's MINIT: mark the entries named in
 * fiber.disable_interceptions disabled and install the rest, in table order. */
void fpm_fiber_intercept_install_all(struct fpm_worker_pool_s *wp);

/* A request's fiber is gone (however it ended); waker is the handle it had
 * (fpm_fiber_io_waker()). Runs every enabled entry's request_end. */
void fpm_fiber_intercept_request_end(void *waker);

#endif
