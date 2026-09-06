/* fpm-ng: pool.executor = fiber — SPIKE, not wired into the build by default
 * outside this branch. Intercepts PHP_STREAM_OPTION_LOCKING on the plain
 * files stream (userland flock() and file_put_contents(..., LOCK_EX), both
 * of which funnel through php_stream_lock() -> php_stream_stdio_ops.set_option,
 * see main/streams/plain_wrapper.c and ext/standard/file.c in php-src) so
 * that a fiber blocked waiting for a lock held by ANOTHER fiber in the SAME
 * process suspends on the scheduler instead of parking the one OS thread in
 * the flock(2) syscall — see docs/flock-streams-spike-report.md for the
 * measured deadlock this closes and docs/flock-fiber-deadlock-report.md
 * (spike/flock-fiber) for the original repro.
 *
 * What this does NOT cover: ext/session's mod_files.c calls flock(2)
 * directly on its own fd, never through the streams API — this hook cannot
 * see that call at all. That is a separate, larger fix (see the report,
 * option 2 in the prior spike) and is out of scope here.
 *
 * Design invariant, load-bearing: the in-process registry NEVER replaces
 * the kernel-level flock() as the source of truth for mutual exclusion.
 * It only decides, for a request that is about to ask for a lock, whether
 * to suspend on a cheap in-process wait queue first so the real flock()
 * attempt (which always still happens, eventually, for every acquire) finds
 * no in-process competition. Every acquire that succeeds went through a real
 * flock()/flock(LOCK_NB) call. If the registry is wrong, disabled, full, or
 * flat out buggy, every code path still falls back to asking the kernel
 * directly — the worst case is parking the process again (today's bug),
 * never silently granting a lock two fibers both believe they hold.
 */

#ifndef FPM_POOL_FIBER_FLOCK_H
#define FPM_POOL_FIBER_FLOCK_H 1

/* Wraps php_stream_stdio_ops.set_option in place. Call once per process,
 * after php_module_startup() (same timing as fpm_pool_fiber_xport_install(),
 * see fpm_pool_fiber_child_main()). No-op if called twice. */
void fpm_pool_fiber_flock_install(void);

/* Releases every lock-registry entry (bookkeeping only, NOT the OS-level
 * flock — that is released by the kernel whenever the underlying fd is
 * closed, independently of this) currently attributed to the fiber
 * identified by `owner` (an opaque handle from fpm_pool_fiber_waiter(),
 * i.e. the SAME value the request's fiber used while it was live) and wakes
 * any other fiber waiting on those files. Call this exactly once when a
 * request's fiber ends, HOWEVER it ends (normal completion, a caught fatal,
 * or "suspended outside the scheduler" / dropped) — a request that never
 * calls this after having taken any lock leaves a permanent phantom holder
 * in the registry, which deadlocks every future request that wants the same
 * file in this process. Safe / cheap no-op if `owner` holds nothing. */
void fpm_pool_fiber_flock_release_owner(void *owner);

#endif
