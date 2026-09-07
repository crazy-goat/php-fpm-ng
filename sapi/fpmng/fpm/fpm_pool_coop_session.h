/* fpm-ng: isolate ext/session state per request in the coop executor (Fiber).
 * See fpm_pool_coop_session.c for rationale and warnings.
 *
 * Rule of this file: zero hard linker dependency on ext/session. There is no
 * ZEND_EXTERN_MODULE_GLOBALS(ps) or reference to ps_globals /
 * session_module_entry by name here — the session module may be statically
 * compiled, loaded as session.so, or not loaded at all, and this file links in
 * all three cases.
 */

#ifndef FPM_POOL_COOP_SESSION_H
#define FPM_POOL_COOP_SESSION_H 1

struct fpm_coop_req_s;

/* Call once from fpm_coop_container_start(), AFTER the container has gone
 * through its own php_request_startup() (INI is registered, and the session
 * module — if present — has completed its OWN container RINIT; EG(ini_directives)
 * and module_registry are populated). A missing session module is not an error:
 * the path is simply disabled (see fpm_coop_session_enabled), and the remaining
 * hooks below cost one boolean check. */
void fpm_coop_session_container_start(void);

/* Call from fpm_coop_req_enter(), in the "if (ctx->live)" block: copy the saved
 * request state (ctx->session_globals) into the session module globals. Call
 * BEFORE resuming/starting the request. No-op when session is not loaded. */
void fpm_coop_session_req_enter(struct fpm_coop_req_s *ctx);

/* Call from fpm_coop_req_leave(), in the "if (ctx->live)" block, BEFORE
 * fpm_coop_base_tables_restore(): copy the CURRENT session globals into
 * ctx->session_globals. Call AFTER the request leaves the processor (suspension
 * or end). No-op when session is not loaded. */
void fpm_coop_session_req_save(struct fpm_coop_req_s *ctx);

/* Call from fpm_coop_base_tables_restore() (next to symbol_table/included_files):
 * session globals <- container base state. No-op when session is not loaded. */
void fpm_coop_session_base_restore(void);

/* Call at the START of fpm_coop_req_run(), next to creation of a fresh symbol
 * table, AFTER setting ctx->live = true: save the base state to session globals
 * and call session-module RINIT on this fresh state — the same RINIT that the
 * classic model calls once per request; here it is called per request despite
 * one php_request_startup() per process. Honors session.auto_start (see the
 * rationale in .c). No-op when session is not loaded. */
void fpm_coop_session_request_startup(void);

/* Call AFTER fpm_coop_execute(), BEFORE destroying EG(symbol_table) — RSHUTDOWN
 * calls php_session_flush() (I/O) and reads $_SESSION, which must still live in
 * the symbol table. Call BEFORE returning globals (ctx->live is still true — a
 * suspension during flush switches through normal fpm_coop_req_leave/enter,
 * transparently). No-op when session is not loaded. */
void fpm_coop_session_request_shutdown(void);

#endif
