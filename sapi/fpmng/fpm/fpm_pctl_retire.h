/* fpm-ng, issue #166 -- THROWAWAY, NOT FOR MERGE.
 *
 * Strategy (a) from spike #66: a scale-down victim whose pool type drains its
 * own connections is asked to retire and then given a bound, instead of being
 * sent SIGQUIT and SIGKILL one maintenance pass apart.
 *
 * The bound lives in the master, keyed by pid, because struct fpm_child_s is
 * upstream's and this branch is not going to patch it. One entry per child the
 * master has asked to retire and has not yet reaped; the table is swept when a
 * child is buried, which is the only way a pid leaves it other than expiry.
 */

#ifndef FPM_PCTL_RETIRE_H
#define FPM_PCTL_RETIRE_H 1

#include <sys/types.h>

/* Drop any bound recorded for `pid`. Called from fpm_children_bury() for every
 * reaped pid, so that a pid the kernel reuses cannot inherit the deadline of
 * the child that held it before. Safe for a pid that is not in the table. */
void fpm_pctl_retire_forget(pid_t pid);

#endif
