/* fpm-ng: tell a forked process that the master has reopened its logs, and
 * hand it the new error_log — issues #134 and #137.
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
 * The datagram is also a wakeup, not only a descriptor (issue #137). The
 * gateway's http.access_log has the same rotation problem and the opposite
 * ownership: the gateway opens that file itself, after dropping privileges,
 * so it can reopen it by path — what it lacks is any way to learn that a
 * rotation happened, because SIGUSR1 is the master's signal and a gateway
 * sets SIGUSR1 to SIG_DFL (fpm_http.c), i.e. signalling the gateway directly
 * would kill it rather than rotate anything. So a notification on this
 * channel means "the master reopened its logs"; what the receiver does with
 * that beyond adopting the descriptor is the receiver's business
 * (fpm_http_log_follow_readable()).
 *
 * That is why the channel is created even when there is no error_log
 * descriptor to hand over — error_log = syslog, where
 * fpm_globals.error_log_fd is ZLOG_SYSLOG. Such a datagram carries no
 * SCM_RIGHTS and the receiving side treats the absence as normal. The
 * rejected alternative was to keep the #134 gate and let http.access_log go
 * unrotated whenever error_log = syslog: a silent gap in the exact
 * configuration where the access log is the only file an operator has.
 *
 * A process that follows nothing and is notified about nothing — the ordinary
 * worker, whose error_log descriptor fpm_stdio_child_use_pipes() takes away —
 * still gets a NULL channel, and every call below is then a no-op.
 *
 * Naming: the module kept its #134 name after #137 widened it, deliberately.
 * A rename to something like fpm_log_reopen.c would describe today's contract
 * better, at the price of detaching every call site, comment and commit that
 * refers to the error_log follow channel from its history.
 */

#ifndef FPM_ERROR_LOG_FOLLOW_H
#define FPM_ERROR_LOG_FOLLOW_H 1

struct fpm_error_log_follow_s;

/* Master, immediately before fork(): a channel for the process about to be
 * forked. NULL when the channel could not be created — that is not a reason
 * to refuse to fork the process, it only means this one will not hear about a
 * reopen. Logs why in that case. */
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

/* Child, from its own event loop: consume every notification waiting on the
 * channel, adopting the error_log descriptor each one carries. Returns how
 * many notifications were consumed — a positive count means the master has
 * reopened its logs and whatever else this process opened for itself is due a
 * reopen too (issue #137). Cannot fail in a way the caller can act on; logs
 * what went wrong. */
int fpm_error_log_follow_child_adopt(void);

/* Master, on SIGUSR1, right after fpm_globals.error_log_fd was re-pointed at
 * a new file: hand that file to every follower. Called for error_log = syslog
 * as well, where there is no descriptor and the datagram is a bare wakeup.
 * Cheap and a no-op with no followers. */
void fpm_error_log_follow_publish(void);

/* Master: the process behind `ch` is gone (reaped, or about to be killed
 * deliberately). NULL-safe. */
void fpm_error_log_follow_free(struct fpm_error_log_follow_s *ch);

#endif
