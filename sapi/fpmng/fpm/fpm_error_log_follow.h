/* fpm-ng: let a forked process follow error_log across a SIGUSR1 reopen —
 * issue #134.
 *
 * The problem. `fpm_stdio_open_error_log(1)` re-points the log in place with
 * dup2(fd, fpm_globals.error_log_fd) (fpm_stdio.c), and only the master ever
 * runs it: SIGUSR1 is handled in the master's event loop. dup2() acts on one
 * process's descriptor table, so a process forked earlier keeps its own copy
 * pointing at the renamed or deleted file. Upstream FPM never hit this,
 * because an ordinary child has no error_log fd at all — fpm_stdio_init_child()
 * closes it, and its comment says exactly why: "child cannot use master
 * error_log because not aware when being reopen".
 *
 * An fpmng HTTP gateway process (fpm_http_gateway_run()) is forked outside
 * that path and does keep the descriptor, deliberately: issue #130 made its
 * lines carry the master's decoration precisely so that it can write them
 * itself. After a logrotate + SIGUSR1 the master writes into the new file
 * while every gateway keeps writing into the old one, and the lines an
 * operator needs — "upstream closed", "pool full" — are simply missing from
 * the file being read.
 *
 * Why the master hands over a descriptor instead of signalling "reopen".
 * The obvious fix is SIGUSR1 to every gateway plus a call to
 * fpm_stdio_open_error_log(1) there. It does not work in the deployment that
 * matters: a gateway drops privileges to the pool's user before it serves
 * anything (fpm_http_gateway_drop_privileges(), task 010), and a distribution
 * error_log is typically root-owned (Debian: /var/log/php8.3-fpm.log, root:adm
 * 0640). open() in the gateway would fail with EACCES, and the diagnostic
 * about it would go into the old file. So this follows the rule the TLS
 * reload machinery already states for the private key: the master owns the
 * file, children never open it (fpm_http_tls_reload.c).
 *
 * The mechanism. One AF_UNIX SOCK_DGRAM socketpair per followed process,
 * created in the master immediately before fork(). After the reopen the
 * master sends its own, already re-pointed fpm_globals.error_log_fd over
 * every one of them as an SCM_RIGHTS ancillary message; the receiver dup2()s
 * it onto its own fpm_globals.error_log_fd — the same number zlog.c's static
 * zlog_fd holds — and closes the received copy. Nothing else in the process
 * changes, so a gateway follows a rotation without dropping a connection.
 *
 * - A pair per process, not one shared channel: a datagram on a socketpair is
 *   delivered to exactly one reader, so a shared channel could not broadcast.
 * - Both ends are non-blocking. The master sends from its event loop and must
 *   never block there; the receiver reads from its own libevent loop.
 * - Lines written by a follower between the master's reopen and its own
 *   adoption of the new descriptor still land in the old file. That window is
 *   one event-loop turn wide and is not worth a synchronous handshake — the
 *   rotated file is still on disk at that point (logrotate copytruncate or
 *   rename), so nothing is lost, it is only in the previous file.
 * - Nothing here is armed when the process has no error_log FILE to follow:
 *   error_log = syslog (fpm_globals.error_log_fd == ZLOG_SYSLOG) and the
 *   ordinary child that has had its descriptor closed both get a NULL channel
 *   and every call below becomes a no-op.
 */

#ifndef FPM_ERROR_LOG_FOLLOW_H
#define FPM_ERROR_LOG_FOLLOW_H 1

struct fpm_error_log_follow_s;

/* Master, immediately before fork(): a channel for the process about to be
 * forked. NULL when there is nothing to follow, or when the channel could not
 * be created — that is not a reason to refuse to fork the process, it only
 * means this one will not follow a reopen. Logs why in the failing case. */
struct fpm_error_log_follow_s *fpm_error_log_follow_new(void);

/* Master, in the parent after fork(): drop the end that belongs to the child.
 * NULL-safe. */
void fpm_error_log_follow_parent(struct fpm_error_log_follow_s *ch);

/* Child, right after fork(): keep `ch`'s receiving end and drop the fds of
 * every other channel, which belong to processes forked before this one.
 * NULL-safe — a child with no channel of its own still has to drop the
 * others'. */
void fpm_error_log_follow_child(struct fpm_error_log_follow_s *ch);

/* Child: the descriptor to watch for readability, -1 when this process is not
 * following anything. Readable means fpm_error_log_follow_child_adopt() has
 * something to do. */
int fpm_error_log_follow_child_fd(void);

/* Child, from its own event loop: adopt every error_log descriptor waiting on
 * the channel. Cannot fail in a way the caller can act on; logs what went
 * wrong. */
void fpm_error_log_follow_child_adopt(void);

/* Master, right after fpm_globals.error_log_fd was re-pointed at a new file:
 * hand that file to every follower. Cheap and a no-op with no followers. */
void fpm_error_log_follow_publish(void);

/* Master: the process behind `ch` is gone (reaped, or about to be killed
 * deliberately). NULL-safe. */
void fpm_error_log_follow_free(struct fpm_error_log_follow_s *ch);

#endif
