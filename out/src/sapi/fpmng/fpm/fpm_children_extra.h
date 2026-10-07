/* fpm-ng: generic registry for master-side processes that are NOT struct
 * fpm_child_s (not counted by pm.*, not FastCGI/PHP workers) but still need
 * the master to notice their death and respawn them — today, the HTTP
 * gateway processes forked by pool.type = http (fpm_http.c).
 *
 * fpm_children_bury() (fpm_children.c, unmodified upstream logic otherwise)
 * reaps EVERY child of the master via waitpid(-1, ...), including these,
 * since they are still real children of the same process. When it does not
 * recognize a pid (fpm_child_find() returns NULL) it used to just log
 * "unknown child ... exited". The one line added to fpm_children.c gives
 * this registry first refusal on that pid before falling back to that log —
 * fpm_children.c itself stays ignorant of what a "gateway" or an "http" pool
 * type even is; it only knows "some pid I did not track asked to be notified".
 */

#ifndef FPM_CHILDREN_EXTRA_H
#define FPM_CHILDREN_EXTRA_H 1

#include <sys/types.h>
#include <sys/time.h>

struct fpm_worker_pool_s;
struct fpm_child_s;

/* Registers `pid`: when fpm_children_bury() reaps it, `on_exit(arg, pid, status)`
 * is called instead of the "unknown child" log line. Called from the master
 * only, right after fork() returns the child's pid to the parent. */
void fpm_children_extra_watch(pid_t pid, void (*on_exit)(void *arg, pid_t pid, int status), void *arg);

/* Stops tracking `pid` without invoking on_exit. Use this right before
 * deliberately killing a tracked process (shutdown, reload) so its exit is
 * not mistaken for a crash and respawned. Safe to call for an untracked pid. */
void fpm_children_extra_forget(pid_t pid);

/* Called by fpm_children_bury() for a reaped pid it does not recognize.
 * Returns 1 and invokes the registered on_exit() if `pid` was being watched
 * (and un-registers it — on_exit is expected to register the replacement
 * process's new pid itself, if any), 0 if `pid` was not being watched. */
int fpm_children_extra_handle_exit(pid_t pid, int status);

/* Issue #329: detach the OLDEST of wp's still-running struct fpm_child_s
 * (smallest ->started) from the pool's own pm.*-counted bookkeeping
 * (wp->children, wp->running_children, fpm_globals.running_children) and
 * return it -- WITHOUT signalling it, closing its fds, or freeing it. The
 * caller becomes responsible for the returned struct: typically it hands the
 * pid straight to fpm_children_extra_watch() above (so its eventual exit,
 * whenever that comes, is still reaped instead of logged as "unknown child")
 * and keeps the struct itself around until then, to free with
 * fpm_children_free() once the pid is confirmed dead.
 *
 * Oldest, not newest or arbitrary: a supervisor pool's rolling reload
 * (fpm_pool_supervisor_reload_spare_child(), fpm_pool_supervisor.c) wants to
 * keep exactly one live copy of the script running across a reload's
 * kill-everyone-then-execvp() cycle, and the copy that has been up longest is
 * the one least likely to be moments from crashing or self-recycling
 * (supervisor.max_memory/max_runtime) on its own -- keeping IT alive gives the
 * survivor window its best chance of actually covering the gap.
 *
 * Returns NULL if wp has no children at all (nothing to detach) -- the
 * caller is expected to have already checked wp->running_children >= 2
 * before calling, since detaching the pool's LAST child would defeat the
 * purpose (no replacement is coming without an execvp() first) and is
 * therefore never done: fpm_pool_supervisor_reload_spare_child() enforces
 * that, this function does not duplicate the check, it merely tolerates the
 * degenerate wp->children == NULL case defensively. */
struct fpm_child_s *fpm_children_detach_oldest(struct fpm_worker_pool_s *wp);

/* Issue #330: the mirror image of fpm_children_detach_oldest() above --
 * registers an already-running pid (one this master did NOT just fork -- a
 * selective reload's carried-over child, see fpm_reload_selective.h) as an
 * ordinary member of wp->children, counted in wp->running_children and
 * fpm_globals.running_children exactly like a forked one, with a fresh
 * scoreboard slot. `started` is the caller's best estimate of when the
 * process actually started; see fpm_reload_selective.c for why "now" is an
 * accepted approximation there. No stdout/stderr pipes are set up
 * (fd_stdout/fd_stderr = -1, same as #329's reload survivor in
 * fpm_pool_supervisor.c and for the same reason: this generation never had
 * pipes open to a pid it did not fork).
 *
 * Returns NULL, doing nothing, if scoreboard registration fails -- the
 * caller treats that exactly like "this pid could not be adopted" and lets
 * the ordinary fork loop that runs right after cover the shortfall. */
struct fpm_child_s *fpm_children_adopt(struct fpm_worker_pool_s *wp, pid_t pid, struct timeval started);

#endif
