/* fpm-ng: see fpm_adopted_sweep.c. Issue #792. */

#ifndef FPM_ADOPTED_SWEEP_H
#define FPM_ADOPTED_SWEEP_H 1

#include <sys/time.h>

struct fpm_child_s;

/* Registers a child that fpm_children_adopt() linked into wp->children for a
 * pid this master did not fork, with the start time that fpm_pid_start_time()
 * read for it (0 when unknown). Arms a once-per-second timer in the master
 * that retires the child when its process is gone. */
void fpm_adopted_sweep_register(struct fpm_child_s *child, unsigned long long start);

#endif
