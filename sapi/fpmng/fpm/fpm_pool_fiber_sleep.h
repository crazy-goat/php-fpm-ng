/* fpm-ng: pool.executor = fiber — sleep()/usleep()/time_nanosleep() without
 * blocking the whole process. SPIKE — see docs/spike-sleep-yield-report.md.
 *
 * Replaces the zif_handler of three internal functions in CG(function_table)
 * with a variant that, when fpm_fiber_io_can_suspend() permits, suspends the
 * request fiber with a TIMER operation through the IO seam instead of calling real
 * sleep()/usleep()/nanosleep() and blocking the whole event loop. Outside the
 * request fiber (or when switching is blocked), calls the original handler
 * unchanged — the real blocking behavior.
 *
 * WHAT THIS DOES NOT COVER (deliberately, see the report):
 *  - pcntl_sleep() — works through SIGALRM/process pause and has no per-request
 *    equivalent in this model (see process-wide pcntl.* functions in
 *    fpm_pool_coop.c; they are disabled in the Fiber container anyway);
 *  - stream_select()/stream_socket_* with a timeout on sockets outside the
 *    tcp/unix transports — that is fpm_pool_fiber_xport.c's territory;
 *  - time_sleep_until() — it is not in the function table when HAVE_NANOSLEEP
 *    is disabled together with time_nanosleep(); when present, it loops over
 *    nanosleep() on EINTR, which cannot loop in this model without a real signal,
 *    but was not covered by this spike (it is not in the task requirements — add
 *    it if needed, using the same pattern below).
 */

#ifndef FPM_POOL_FIBER_SLEEP_H
#define FPM_POOL_FIBER_SLEEP_H 1

#include "fpm_pool_fiber_io.h"

/* Registry entry "sleep" (fpm_pool_fiber_intercept.c). Its install runs ONCE
 * per Fiber-pool child; missing functions (for example, nanosleep unavailable
 * on the platform) are skipped with a log warning — never a fatal error. */
extern struct fpm_fiber_intercept_s fpm_fiber_sleep_intercept;

#endif
