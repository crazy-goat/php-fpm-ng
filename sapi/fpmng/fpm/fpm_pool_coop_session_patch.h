/* fpm-ng: SECOND variant of the in-process session-lock arbiter — patches
 * PS(mod) (ps_globals.mod) in place instead of registering a differently
 * named save-handler module. See fpm_pool_coop_session_lock.[ch] for the
 * first variant ("files_arb") and docs/session-lock-arbiter-report.md for
 * the comparison and trade-offs between the two.
 *
 * Point of this variant: protect plain `session.save_handler = files` with
 * ZERO pool reconfiguration and with NO new link-time dependency on
 * php_session_register_module()/_php_find_ps_module() — at the cost of
 * overwriting a struct field (ps_globals.mod) that is not a documented
 * extension point, using a name-based heuristic to recognize the built-in
 * "files" module, and (see the .c file) a real gap: it cannot protect the
 * FIRST session_start() call of a request when session.auto_start = 1,
 * because that call happens synchronously inside RINIT, before this file's
 * post-RINIT hook ever runs.
 */

#ifndef FPM_POOL_COOP_SESSION_PATCH_H
#define FPM_POOL_COOP_SESSION_PATCH_H 1

struct fpm_coop_req_s;

/* Resolves ps_globals's address (own copy of the ini-entry mh_arg2 trick,
 * same technique as fpm_pool_coop_session.c, duplicated here so this file
 * has no compile-time dependency on that one) and captures the built-in
 * "files" module's function pointers by reading them straight out of
 * ps_globals.mod at this exact moment — BEFORE the container's own
 * php_request_startup() ever runs, i.e. before this process's first RINIT
 * has had a chance to touch the field. Call once, in
 * fpm_coop_container_start(), before php_request_startup(). No-op (logs
 * once at WARNING/NOTICE) if session isn't loaded, the ini-entry trick
 * doesn't resolve, or ps_globals.mod isn't already the built-in "files"
 * module at that point (e.g. a pool whose default save_handler isn't
 * "files" - this variant then stays permanently inert for that pool, it
 * does not retry later). */
void fpm_coop_session_patch_container_start(void);

/* Call right after fpm_coop_session_request_startup() (i.e. right after
 * ext/session's real per-request RINIT has run) in fpm_coop_req_run(). If
 * ps_globals.mod was just resolved (by RINIT) to the built-in "files"
 * module, overwrites it with this file's wrapper module so every
 * s_read()/s_close() for the rest of this request goes through the
 * in-process arbiter. No-op otherwise (redis/memcached/user handler, or the
 * capture at container start never succeeded). */
void fpm_coop_session_patch_req_apply(void);

/* Self-healing re-check: call from fpm_coop_req_enter(), i.e. on EVERY
 * fiber resume within a request's lifetime, not just its first entry. If a
 * script calls ini_set('session.save_handler', 'files') (or 'files' again,
 * having switched away and back) mid-request, ext/session's own
 * OnUpdateSaveHandler ini callback (session.c) overwrites ps_globals.mod
 * directly, silently discarding whatever this file installed there —
 * something this project cannot intercept (it lives in php-src). This hook
 * re-detects that situation at the next scheduler checkpoint and re-applies
 * the wrapper, logging a WARNING the first time it happens per process (see
 * the .c file for why "silently do nothing" was rejected: losing this
 * protection with no trace in the logs is worse than not having it). */
void fpm_coop_session_patch_req_enter(struct fpm_coop_req_s *ctx);

/* Leak safety net, same pattern/reasoning as
 * fpm_coop_session_lock_req_free() in the files_arb variant. */
void fpm_coop_session_patch_req_free(struct fpm_coop_req_s *ctx);

#endif
