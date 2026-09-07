/* fpm-ng: isolate ini-entry VALUES (ini_set/ini_get) per request in the coop
 * executor (Fiber). See fpm_pool_coop_ini.c for rationale, scope, and limits.
 */

#ifndef FPM_POOL_COOP_INI_H
#define FPM_POOL_COOP_INI_H 1

struct fpm_coop_req_s;

/* Call from fpm_coop_req_enter(), in the "if (ctx->live)" block: restore this
 * request's OWN value into ini entries it changed before its last leave from
 * the processor. Call BEFORE resuming/starting the request. Cheap path:
 * ctx->ini_mods == NULL (request never changed ini) does NOTHING — no
 * allocation and no table walk. */
void fpm_coop_ini_req_enter(struct fpm_coop_req_s *ctx);

/* Free what remains after a request was destroyed while off the processor (ini
 * entries are already back to baseline; fpm_coop_ini_req_leave restored them). */
void fpm_coop_ini_req_free(struct fpm_coop_req_s *ctx);

/* Call from fpm_coop_req_leave(), in the "if (ctx->live)" block: remove from
 * EG(modified_ini_directives) everything THIS request changed since its last
 * entry, hide the request's OWN value in ctx, and restore each entry's baseline
 * value (what the container/another request would see). Call AFTER the request
 * leaves the processor (suspension — NOT request end; see fpm_pool_coop.c: at
 * request end ctx->live is already false, so this hook is not called and final
 * restoration is done by zend_ini_deactivate()). Cheap path:
 * EG(modified_ini_directives) == NULL (request changed nothing since its last
 * entry) does NOTHING. */
void fpm_coop_ini_req_leave(struct fpm_coop_req_s *ctx);

#endif
