/* fpm-ng: a log channel from a child back to the master, for pool types whose
 * whole policy runs in the child.
 *
 * A classic FastCGI child has nothing of its own to say: everything an
 * operator needs about it (started, exited, terminated) is logged by the
 * master, which is why upstream deliberately takes the error_log away from a
 * child (fpm_stdio_init_child(): close(error_log_fd) + zlog_set_fd(-1) — "child
 * cannot use master error_log because not aware when being reopen"). zlog()
 * then falls back to STDERR_FILENO, which with the default
 * catch_workers_output = no is the master's stdout, which fpm_stdio_init_main()
 * has already pointed at /dev/null.
 *
 * pool.type = supervisor and pool.type = cron break that assumption: their
 * policy — backoff, restart_max, give-up, cron timeouts, "cannot open script" —
 * runs inside the child (fpm_pool_supervisor_child_main(), fpm_pool_cron_child_main()),
 * so the messages an operator needs are emitted there. Before this file, a
 * supervised script that crash-looped in production was silent unless the pool
 * also set catch_workers_output = yes, and even then every message arrived
 * wrapped in a master-side WARNING "child N said into stderr: ..." at the wrong
 * level — issue #121.
 *
 * The channel is one AF_UNIX SOCK_DGRAM socketpair per POOL, created in the
 * master before its first child is forked and kept for the master's whole life:
 *
 * - The master holds BOTH ends open, so no child's death ever produces an EOF
 *   the master would have to notice and clean up: a channel is per pool, not
 *   per child, and every child of the pool inherits the same write end. There
 *   is no in-process config reload to invalidate it either — FPM re-execs
 *   itself for a reload (fpm_pctl_exec()).
 * - SOCK_DGRAM, not a pipe: one write() is one message, so a message can never
 *   be split, and messages from several children of the same pool cannot
 *   interleave into each other. That is also why the master needs no framing
 *   or line buffering, unlike fpm_stdio_child_said().
 * - The master re-emits each record through its own zlog() at the level the
 *   child used, with the sending child's pid appended as "(child N)", so the
 *   line lands in error_log (or syslog) looking exactly like a master line:
 *   right level, right timestamp, and reopened by SIGUSR1 like everything else,
 *   because the fd still belongs to the master. The pid is in the record
 *   because the channel is per pool — with supervisor.processes > 1 the text of
 *   the lines is identical.
 *
 * Nothing here knows what a supervisor is: a pool type opts in with
 * fpm_pool_type_s.child_logs_via_master (see fpm_pool_type.h).
 *
 * TRAP for child-side code in such a pool: ZLOG_SYSERROR does not survive the
 * relay. zlog()'s external-logger hook is called with the formatted message
 * only (zlog_external(), zlog.c), and the ": %s (%d)" strerror(errno) suffix is
 * appended afterwards, in vzlog() — so a ZLOG_SYSERROR emitted in the child
 * reaches error_log as a plain ERROR with the reason stripped. Write
 * strerror(errno) into the format yourself, the way fpm_pool_cron.c already
 * does.
 */

#ifndef FPM_CHILD_LOG_H
#define FPM_CHILD_LOG_H 1

struct fpm_child_s;
struct fpm_worker_pool_s;

/* Master, before fork(): make sure the pool of `child` has a channel. Returns
 * 0 even when the channel could not be created — losing the pool's log is bad,
 * but it is not a reason to refuse to start the supervised process, which is
 * the thing that actually does the work. Logs why in that case. */
int fpm_child_log_prepare(struct fpm_child_s *child);

/* Master, right after fork(): register the read end in the event loop. Cheap
 * and idempotent — only the first child of the pool actually registers. */
void fpm_child_log_parent_use(struct fpm_child_s *child);

/* Child, right after fork(): keep this pool's write end, drop every fd of
 * every channel that belongs to another pool. */
void fpm_child_log_child_use(struct fpm_child_s *child);

/* Child, from fpm_stdio_init_child(): route this child's zlog() into the
 * channel. Must run AFTER fpm_stdio_init_child() has done its own
 * zlog_set_fd(), which it would otherwise undo. Cannot fail in a way worth
 * refusing to start the supervised process over — see the /dev/null comment in
 * the implementation. */
void fpm_child_log_init_child(struct fpm_worker_pool_s *wp);

#endif
