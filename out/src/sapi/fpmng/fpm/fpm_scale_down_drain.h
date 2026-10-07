/* fpm-ng: issue #310. A pid-keyed registry the master uses across two files
 * that must not touch struct fpm_child_s or fpm_process_ctl.h (both upstream
 * php-src, unmodified by this SAPI) to carry one thing neither has room for:
 * when a scale-down first told THIS child to drain, so the next maintenance
 * pass can grant it the pool's own http.read_timeout before escalating to
 * SIGKILL, instead of the next pass ~1s later
 * (FPM_IDLE_SERVER_MAINTENANCE_HEARTBEAT).
 *
 * Implemented in fpm_process_ctl.c, the only writer. fpm_children.c only
 * needs the forget half, called for every reaped pid regardless of whether it
 * was tracked, so a pid the OS reuses can never inherit a stale deadline.
 */

#ifndef FPM_SCALE_DOWN_DRAIN_H
#define FPM_SCALE_DOWN_DRAIN_H 1

#include <sys/types.h>

/* Un-tracks `pid`, if tracked. Safe to call for any reaped pid. */
void fpm_scale_down_drain_forget(pid_t pid);

#endif
