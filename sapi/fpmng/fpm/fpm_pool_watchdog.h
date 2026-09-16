/* fpm-ng: shared pidfd-based "send this process a signal if it's still alive
 * after N seconds" watchdog. Used by pool.type = supervisor
 * (supervisor.stop_timeout, a safety net after SIGTERM, and
 * supervisor.max_runtime, a cap on a single script iteration — see
 * fpm_pool_supervisor.c) and pool.type = cron (cron.timeout, a cap on how long
 * a single run may take). Extracted here because all three need exactly the
 * same mechanism, only with a different trigger, a different timeout value,
 * and (for max_runtime) a different signal — see docs/NOTES.md 3o for the
 * original writeup and the reasoning behind pidfd over a plain remembered PID.
 */

#ifndef FPM_POOL_WATCHDOG_H
#define FPM_POOL_WATCHDOG_H 1

#include <sys/types.h>

/* Forks a small watchdog process that waits up to `timeout_seconds` for
 * `target_pid` to terminate (any way: normal exit, signal, whatever) and, if
 * it is still alive when the timer fires, sends it `signo`. If the target
 * terminates on its own before the timer fires, the watchdog notices and
 * exits quietly without sending anything.
 *
 * Most callers use signo = SIGKILL and never need to cancel: a single arm per
 * process lifetime, coinciding with (or causing) that process's end, so there
 * is nothing left to cancel by the time it would matter. supervisor.max_runtime
 * is the one caller that arms this repeatedly WITHIN one long-lived process
 * (once per for(;;) loop iteration) with signo = the "please stop" signal
 * (SIGTERM today; supervisor.stop_signal once issue #324 lands) rather than
 * SIGKILL directly — the existing per-pool SIGTERM handler then arms a SECOND
 * watchdog with signo = SIGKILL, exactly the same as an externally requested
 * stop, so the hard kill is never skipped. Because the target process does
 * NOT terminate between iterations, that caller MUST explicitly cancel a
 * watchdog armed for an iteration that finished on time (kill() the returned
 * pid, then waitpid() it) before arming the next one — otherwise a slow
 * iteration would leave a stale watchdog counting from the WRONG start time
 * that fires during a later, well-behaved iteration. See
 * fpm_pool_supervisor_child_main().
 *
 * On Linux this is done with pidfd_open()/pidfd_send_signal() + poll(),
 * which refers to the exact process instance and is immune to PID reuse.
 * Elsewhere (only relevant for local builds/tests, never the target
 * container platform) it falls back to polling kill(pid, 0) once a second,
 * with a narrow, documented PID-reuse race.
 *
 * Async-signal-safe: every function this calls (fork(), poll(), syscall(),
 * close(), sleep(), kill(), _exit()) is on the POSIX async-signal-safe list,
 * so this may be called directly from a signal handler (that is what
 * pool.type = supervisor does for its stop_timeout).
 *
 * Returns the watchdog's PID (>0) on success, or -1 on fork() failure — the
 * caller proceeds either way, this is a best-effort safety net, not a
 * correctness requirement.
 */
pid_t fpm_pool_watchdog_arm(pid_t target_pid, unsigned timeout_seconds, int signo);

#endif
