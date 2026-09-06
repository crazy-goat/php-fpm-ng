/* fpm-ng: per-request isolation of a CONFIGURED list of class static
 * properties on the coop (fiber) executor. See fpm_pool_coop_statics.c for
 * the design, the memory-ownership argument, and the hazards it was checked
 * against (tasks/008-laravel-class-statics.md).
 */

#ifndef FPM_POOL_COOP_STATICS_H
#define FPM_POOL_COOP_STATICS_H 1

#include "fpm_pool_coop.h"

struct fpm_worker_pool_s;

/* Syntax-only check of pool.config->fiber_isolate_statics, called from
 * fpm_pool_type_fiber_validate() (fpm_pool_type.c), i.e. on the master side,
 * before any child forks. Rejects a directive that cannot possibly be a list
 * of "Class\Name::property" entries -- this is the failure mode that must
 * stop the pool from starting rather than run isolation-disabled (an admin
 * who wrote a syntax error should see it immediately, exactly like
 * fpm_http_acl_parse() failing pool.type = http on a malformed ACL). Class
 * existence and "is it actually static" cannot be checked here: the
 * autoloader has not run yet. 0 or -1. */
int fpm_coop_statics_validate(struct fpm_worker_pool_s *wp);

/* Builds the runtime item list from pool.config->fiber_isolate_statics.
 * Call once, in fpm_coop_container_start(), after php_admin_value has been
 * applied and before the first request -- same point fpm_coop_session_
 * container_start() is called from. Empty/missing directive: 0 items, every
 * hook below becomes a single "if" (Q5 in the spike, still true here). */
void fpm_coop_statics_container_start(const char *pool_name);

/* Live static property slots -> ctx (this request is leaving the CPU).
 * Cheap no-op when the item list is empty. */
void fpm_coop_statics_req_leave(struct fpm_coop_req_s *ctx);

/* ctx -> live static property slots (this request is about to run).
 * Cheap no-op when the item list is empty or ctx never stashed anything. */
void fpm_coop_statics_req_enter(struct fpm_coop_req_s *ctx);

/* Releases anything a request's ctx is still holding when it is destroyed
 * without a matching req_enter (worker shutdown mid-suspension). No-op in
 * the overwhelmingly common case (ctx holds nothing). */
void fpm_coop_statics_req_free(struct fpm_coop_req_s *ctx);

#endif
