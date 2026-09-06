/* fpm-ng: in-process arbiter for ext/session's "files" save handler under
 * pool.executor = fiber — SPIKE, see docs/session-lock-arbiter-report.md.
 *
 * Problem: ext/session's built-in "files" save handler takes a real,
 * blocking flock(2) on a raw fd (ext/session/mod_files.c, ps_files_open()),
 * entirely outside the streams layer. Two fibers in the SAME process with
 * the same PHPSESSID deadlock the whole worker: one fiber holds the lock and
 * is suspended elsewhere (e.g. a slow socket read), the other calls
 * session_start() and blocks the process's one OS thread inside the kernel
 * flock() wait queue — the scheduler can never run again, so the lock holder
 * never resumes to release it either. See docs/flock-fiber-deadlock-report.md
 * (branch spike/flock-fiber) for the measured deadlock.
 *
 * Fix here: narrow the kernel-level contention down to "at most one fiber
 * per process ever calls the real flock() for a given session id at a time"
 * by adding an in-process, per-session-id lock ahead of it, using the
 * project's own fiber wait/wake primitives (fpm_pool_fiber.h) instead of a
 * blocking syscall. Cross-process contention for the same id is UNCHANGED
 * (still the real flock(), still correctly serializes — see the report's
 * cross-process measurement).
 *
 * See fpm_pool_coop_session_lock.c for why this can NOT be done by
 * re-registering a module also named "files" (verified against
 * ext/session/session.c: the built-in "files" module is hard-coded at
 * ps_modules[0] and _php_find_ps_module() always matches it first) — it
 * requires a DIFFERENTLY NAMED module ("files_arb") plus an explicit
 * `session.save_handler = files_arb` in pool config. Nothing protects a pool
 * left at `session.save_handler = files`.
 */

#ifndef FPM_POOL_COOP_SESSION_LOCK_H
#define FPM_POOL_COOP_SESSION_LOCK_H 1

struct fpm_coop_req_s;

/* Registers the "files_arb" save handler and captures the real "files"
 * module's function pointers to delegate to. Call once, in
 * fpm_coop_container_start(), BEFORE the container's own
 * php_request_startup() — ext/session's real PHP_RINIT_FUNCTION resolves
 * session.save_handler via _php_find_ps_module() fresh on EVERY request
 * (verified: session.c's php_rinit_session() sets PS(mod) = NULL then looks
 * it up again unconditionally), so our module must already be registered
 * before the very first RINIT this process ever runs. No-op if ext/session
 * is not loaded, if the real "files" module cannot be found, or if the
 * module table is full (logs once at WARNING either way; the pool then
 * behaves exactly as it does today, unprotected). */
void fpm_coop_session_lock_container_start(void);

/* Records "the request ctx currently on the CPU" — needed because
 * PS_READ_ARGS/PS_CLOSE_ARGS carry no request identity, so a lock acquired
 * deep inside the save-handler callback is attributed to whichever ctx is
 * running at that moment. Call from fpm_coop_req_enter(), same place as the
 * existing fpm_coop_session_req_enter() etc. No-op if the arbiter isn't
 * installed. */
void fpm_coop_session_lock_req_enter(struct fpm_coop_req_s *ctx);

/* Leak safety net: if ctx is being torn down (fiber died/destroyed, e.g. a
 * bailout) while still attributed as the holder of an in-process session
 * lock, force-release it and wake the next in-process waiter, if any — a
 * leaked entry here would deadlock the process PERMANENTLY (nobody could
 * ever pass this session id's in-process gate again), which is exactly the
 * class of bug this file exists to remove, not reintroduce. Call from
 * fpm_coop_req_free(), same pattern as the existing fpm_coop_ini_req_free() /
 * fpm_coop_statics_req_free(). Does NOT touch the real kernel flock/fd —
 * that is owned by ps_globals.mod_data for this request and, on an abnormal
 * fiber death, may leak independently of this feature (a pre-existing risk
 * shared with the unmodified coop-session code, not introduced here). */
void fpm_coop_session_lock_req_free(struct fpm_coop_req_s *ctx);

#endif
