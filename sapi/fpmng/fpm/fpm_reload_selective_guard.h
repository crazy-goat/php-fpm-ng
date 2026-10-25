/* fpm-ng: issue #829 -- the guard for the workers a selective reload spares.
 *
 * A reload with reload.selective = yes hands an unchanged pool's workers to
 * the new master in FPMNG_SELECTIVE_RELOAD_SURVIVORS (fpm_reload_selective.h),
 * and that master adopts them. If the new master is killed outright (SIGKILL,
 * OOM, a crash) before it does, nothing signals those workers: they keep their
 * listening sockets, and the port stays held by a process with no master.
 *
 * The guard is a small process that lives from the old master's execvp() until
 * every entry is adopted or discarded. It holds the read end of a pipe. The
 * write end is held by the master generation: it has no close-on-exec, so it
 * survives execvp() and the daemonize fork. The new master writes one byte and
 * closes its end when it releases the guard (fpm_reload_selective_discard_unadopted(),
 * which runs after the initial fork loop). The old master does the same when
 * both exec attempts fail. The guard reads that byte and exits.
 * A child the new master forks before the release closes its copy at once
 * (fpm_reload_selective_guard_child_init()). If every holder dies first, the
 * guard reads end of file and sends SIGTERM to the carried workers, checking
 * each pid's start time (fpm_pid_is_same(), issue #891).
 *
 * Rejected alternatives:
 * - getppid() or PR_SET_PDEATHSIG in the worker: under daemonize the pre-fork
 *   parent exits after fpm_init(), before fpm_run() adopts the workers, so the
 *   ppid of a spared worker is already 1 on a normal reload.
 * - A pid file of the master: optional, and stale after a crash.
 * - A lock (flock, fcntl) on a file: the same lifetime question, plus a file to
 *   clean up.
 * - A direct fork of the guard: the new master would log "unknown child ...
 *   potentially a bug" under -F (fpm_children.c, fpm_children_bury()). The guard
 *   is a grandchild, so the intermediate process reaps the guard's fork.
 *
 * Known gaps, follow-ups and not part of this design: a new master that hangs
 * instead of dying; a master killed after the release (the guard is gone, and
 * its adopted children are orphans as they were before); the #329 survivors of
 * a master that is killed while running; a binary that predates this guard
 * (the guard then waits until that generation is gone, and only signals pids
 * that still match their start time). */

#ifndef FPM_RELOAD_SELECTIVE_GUARD_H
#define FPM_RELOAD_SELECTIVE_GUARD_H 1

/* Called from fpm_pctl_exec() (fpm_process_ctl.c) in the old master, before the
 * cleanups and the execvp(). Does nothing unless the carried list is not empty.
 * Forks the guard (double fork) and keeps the write end. On any failure it logs
 * a WARNING and carries on without a guard: the reload itself is not affected. */
void fpm_reload_selective_guard_arm(void);

/* Called from fpm_init() in the new master, before any fork. Takes the write end
 * out of the environment and keeps it in this process. The children must use
 * that copy: a child clears the environment (clear_env) before the child hook
 * runs. A no-op when no guard was armed. */
void fpm_reload_selective_guard_init_main(void);

/* Called from fpm_reload_selective_discard_unadopted() in the new master, after
 * the SIGTERMs for the carried entries that no pool adopted, and in the old
 * master when both exec attempts fail, before it exits. Writes one byte to the
 * pipe and closes the write end. A no-op when no guard was armed. */
void fpm_reload_selective_guard_release(void);

/* Called in every child the master forks (fpm.c, fpm_http_gw_proc.c), right
 * after fpm_reload_shm_child_init(). Closes the child's copy of the write end,
 * so that the guard does not wait for a process that is not the master. After
 * the release the master holds no write end, so a child forked later does
 * nothing. A future fork site that keeps the write end makes the guard miss
 * its signal, never send one by mistake. */
void fpm_reload_selective_guard_child_init(void);

#endif
