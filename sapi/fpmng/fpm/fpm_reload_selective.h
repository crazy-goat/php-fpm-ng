/* fpm-ng: issue #330 -- selective reload's carry-the-children-across-execvp()
 * mechanism.
 *
 * A reload in this codebase (like upstream's) is a full execvp() of the
 * master once every pool's children have exited (fpm_process_ctl.c) -- the
 * NEW generation boots from scratch and re-forks every pool's children from
 * zero, the same way a cold start does. That is exactly the "restart" this
 * issue wants reload.selective = yes to spare an UNCHANGED pool from: not
 * just "signalled a little later" or "signalled with a gentler signal" but
 * genuinely not restarted, its worker pids identical before and after.
 *
 * Getting there needs the OLD generation (about to execvp()) to hand the
 * NEW one (right after) enough information to skip forking replacements for
 * an unchanged pool and adopt its still-running children instead. Shared
 * memory does not survive execve() (fpm_shm_alloc()'s mapping is
 * MAP_ANONYMOUS -- see issue #329's fpm_pool_supervisor.c for the same fact
 * used the same way), so, like #329, this hands the pids across in an
 * environment variable, which execvp() does preserve.
 *
 * This generalizes #329's single-spared-child-per-supervisor-pool mechanism
 * (fpm_pool_supervisor.c's FPM_SUPERVISOR_RELOAD_SURVIVOR_ENV) into ALL of an
 * unchanged pool's children, for every pool type -- not a pool-type callback,
 * because unlike #329 (which is about giving a graceful, staggered handoff to
 * a pool that IS restarting) this is about a pool that never restarts at
 * all, which is the same decision regardless of what the pool runs. */

#ifndef FPM_RELOAD_SELECTIVE_H
#define FPM_RELOAD_SELECTIVE_H 1

struct fpm_worker_pool_s;

/* Called from fpm_pctl_kill_all() (fpm_process_ctl.c), once per pool, on a
 * reload's first signal pass, ONLY for a pool fpm_conf_diff_pool_unchanged()
 * says is unchanged. Detaches every one of wp's children from the pool's own
 * pm.*-counted bookkeeping (so the ordinary signal fan-out that follows never
 * reaches them -- the caller does not add this pool to that loop at all, see
 * its own comment) and records each pid in FPM_RELOAD_SELECTIVE_ENV for the
 * next generation to adopt. Does not signal, close, or free anything: an
 * unchanged pool's children are not being asked to do anything at all. */
void fpm_reload_selective_spare_pool(struct fpm_worker_pool_s *wp);

/* Called from fpm_children_create_initial() (fpm_children.c), once per pool,
 * before that pool's normal cold-start fork loop runs. Consumes wp's entry
 * (if any) from FPM_RELOAD_SELECTIVE_ENV and adopts each still-alive pid as
 * an ordinary running child of wp (fpm_children_adopt(), fpm_children.c) --
 * counted in wp->running_children exactly like a forked one, so the fork loop
 * that runs immediately after this only tops up the shortfall, which for a
 * genuinely unchanged pool is zero. A pid that died in the brief window
 * between being spared and this call (the OLD generation exits almost
 * immediately after sparing it, so the window is the time execvp() plus this
 * generation's own startup takes) is silently skipped -- the ordinary fork
 * loop covers it exactly as it would a crashed ordinary child. */
void fpm_reload_selective_adopt(struct fpm_worker_pool_s *wp);

#endif
