/* fpm-ng: shared pidfd-based "kill this process if it's still alive after
 * N seconds" watchdog. Used by pool.type = supervisor (supervisor.stop_timeout,
 * a safety net after SIGTERM) and pool.type = cron (cron.timeout, a cap on
 * how long a single run may take). Extracted here because both need exactly
 * the same mechanism, only with a different trigger and a different timeout
 * value — see docs/NOTES.md 3o for the original writeup and the reasoning
 * behind pidfd over a plain remembered PID.
 */

#ifndef FPM_POOL_WATCHDOG_H
#define FPM_POOL_WATCHDOG_H 1

#include <sys/types.h>

/* Forks a small watchdog process that waits up to `timeout_seconds` for
 * `target_pid` to terminate (any way: normal exit, signal, whatever) and, if
 * it is still alive when the timer fires, kills it with SIGKILL. If the
 * target terminates on its own before the timer fires, the watchdog notices
 * and exits quietly without sending anything — no explicit "cancel" call is
 * needed by the caller.
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
pid_t fpm_pool_watchdog_arm(pid_t target_pid, unsigned timeout_seconds);

#endif
