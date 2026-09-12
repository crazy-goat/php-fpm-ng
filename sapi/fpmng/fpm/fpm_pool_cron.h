/* fpm-ng: pool.type = cron — a script run on a crontab-style schedule,
 * one run per process lifetime. See fpm_pool_cron.c and docs/NOTES.md for
 * the design writeup (in particular: why there is deliberately no timer and
 * no shared state on the master side, and why overlap is impossible by
 * construction rather than by policy).
 */

#ifndef FPM_POOL_CRON_H
#define FPM_POOL_CRON_H 1

struct fpm_worker_pool_s;

/* Directives rejected for pool.type = cron. NULL-terminated, used as
 * .rejects in fpm_pool_types[]. */
extern const char *const fpm_pool_cron_rejects[];

/* fpm_pool_type_s.validate — cron.schedule/cron.script required; schedule is
 * parsed ONCE here (bad schedule = configuration rejected at startup, not
 * interpreted "approximately" at runtime), mapping to pm = static +
 * pm.max_children = 1. */
int fpm_pool_cron_validate(struct fpm_worker_pool_s *wp);

/* fpm_pool_type_s.init_main — allocates ONLY what the operator pages need to
 * show last_run/last_exit_code (docs/NOTES.md 3u). Cron still has NO
 * restart/backoff policy that must survive process death (see the rationale at
 * the top of fpm_pool_cron.c); this is READ-ONLY state for the pages, never state
 * that controls cron behavior itself. */
int fpm_pool_cron_init_main(struct fpm_worker_pool_s *wp);

/* fpm_pool_type_s.child_main — calculates the next due time, sleeps until it
 * (interruptibly by SIGTERM), runs the script once, and exits. Does not return.
 * Respawn for the next run is the ordinary unconditional fpm_children.c respawn
 * (pm = static, max_children = 1); cron policy itself still does not depend on
 * shared memory, unlike supervisor (see init_main above: the added state is
 * ONLY for status, not control). */
void fpm_pool_cron_child_main(struct fpm_worker_pool_s *wp);

struct fpm_pool_status_s;

/* fpm_pool_type_s.status — state for this pool as the operator pages read it.
 * last_run/last_exit_code come from shared memory; next_run is calculated ON
 * DEMAND from the schedule and current clock (not from any stored state) — see
 * docs/NOTES.md 3u. */
void fpm_pool_cron_status(struct fpm_worker_pool_s *wp, struct fpm_pool_status_s *out);

#endif
