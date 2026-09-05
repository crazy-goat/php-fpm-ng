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

#endif
