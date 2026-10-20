/* fpm-ng: see fpm_pid_child.c. Issue #891. */

#ifndef FPM_PID_CHILD_H
#define FPM_PID_CHILD_H 1

#include <sys/types.h>

/* Start time of `pid`, in clock ticks since boot, or 0 when it cannot be read
 * (no /proc, or no such process). A spare records it while it still holds the
 * pid, and the next generation checks it with fpm_pid_is_same(). */
unsigned long long fpm_pid_start_time(pid_t pid);

/* Issue #792: reads the state letter (field 3 of /proc/<pid>/stat) and the
 * parent pid (field 4). Returns 0 when they cannot be read (no /proc, or no
 * such process). A process that exited but whose parent has not reaped it yet
 * is `Z`: its /proc entry and its start time are still there, so
 * fpm_pid_is_same() reports it as the same live process. */
int fpm_pid_proc_state(pid_t pid, char *state, pid_t *ppid);

/* Returns 1 when `pid` is still the process whose start time was `start`. A
 * start of 0 means none was recorded: then only liveness is checked. */
int fpm_pid_is_same(pid_t pid, unsigned long long start);

#endif
