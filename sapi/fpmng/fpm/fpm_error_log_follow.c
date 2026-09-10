/* fpm-ng: see fpm_error_log_follow.h — issue #134. */

#include "fpm_config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fpm.h"
#include "fpm_sockets.h"
#include "fpm_error_log_follow.h"
#include "zlog.h"

/* At most this many descriptors are adopted per readable event. A rotation
 * sends one datagram per follower channel, so in practice there is never more
 * than one waiting; the bound only stops a pathological sender from keeping
 * the gateway's event loop in this function. The channel stays readable, so a
 * leftover is picked up on the next turn — the event is level-triggered. */
#define FPM_ERROR_LOG_FOLLOW_BATCH 8

/* Linux since 2.6.23; absent on platforms fpmng does not target (it needs
 * epoll through libevent anyway). Zero there just means the received
 * descriptor is exec()able for the few lines it exists. */
#ifdef MSG_CMSG_CLOEXEC
# define FPM_ERROR_LOG_FOLLOW_RECV_FLAGS MSG_CMSG_CLOEXEC
#else
# define FPM_ERROR_LOG_FOLLOW_RECV_FLAGS 0
#endif

struct fpm_error_log_follow_s {
	struct fpm_error_log_follow_s *next;
	int fd_master;			/* the master sends the reopened file on this one */
	int fd_child;			/* the followed process receives it here; -1 in the master once it has forked */
};

/* Master side: every channel whose process is still alive. Only the master's
 * single-threaded event loop touches this, so a plain list needs no locking —
 * the same argument fpm_children_extra.c makes. */
static struct fpm_error_log_follow_s *followers = NULL;

/* Child side: this process's receiving end, -1 when it follows nothing. */
static int fpm_error_log_follow_fd = -1;

/* The payload is never read for its content — SCM_RIGHTS is the message. It
 * exists because a zero-length datagram is not portably distinguishable from
 * end-of-file on the receiving side. */
static const char fpm_error_log_follow_byte = 'r';

/* EAGAIN and EWOULDBLOCK are the same value on most systems, hence the macro
 * dance — the same one fpm_http_would_block() does, and for the same reason:
 * gcc's -Wlogical-op rejects testing both. */
static inline int fpm_error_log_follow_would_block(int err)
{
	if (err == EAGAIN) {
		return 1;
	}
#if EWOULDBLOCK != EAGAIN
	if (err == EWOULDBLOCK) {
		return 1;
	}
#endif
	return 0;
}

static void fpm_error_log_follow_close(struct fpm_error_log_follow_s *ch) /* {{{ */
{
	if (ch->fd_master >= 0) {
		close(ch->fd_master);
		ch->fd_master = -1;
	}
	if (ch->fd_child >= 0) {
		close(ch->fd_child);
		ch->fd_child = -1;
	}
}
/* }}} */

struct fpm_error_log_follow_s *fpm_error_log_follow_new(void) /* {{{ */
{
	struct fpm_error_log_follow_s *ch;
	int fds[2];

	/* Nothing to follow: error_log = syslog (ZLOG_SYSLOG, negative) or a
	 * process without an error_log FILE. Same gate as
	 * fpm_child_error_log_use(), and for the same reason — see its comment. */
	if (fpm_globals.error_log_fd <= 0) {
		return NULL;
	}

	if (0 > socketpair(AF_UNIX, SOCK_DGRAM, 0, fds)) {
		zlog(ZLOG_SYSERROR, "failed to create the error_log follow channel; "
			"this process will keep writing into the pre-rotation error_log");
		return NULL;
	}

	/* Both ends non-blocking: the master sends from its event loop and the
	 * follower receives from its own, and neither may block there. A send
	 * that would block is a lost rotation for that one process, which is
	 * strictly better than a stalled master. */
	if (0 > fd_set_blocked(fds[0], 0) || 0 > fd_set_blocked(fds[1], 0)) {
		zlog(ZLOG_SYSERROR, "failed to unblock the error_log follow channel");
		close(fds[0]);
		close(fds[1]);
		return NULL;
	}

	/* Nothing this process may exec() later has any business holding the
	 * master's error_log channel. */
	fcntl(fds[0], F_SETFD, fcntl(fds[0], F_GETFD) | FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, fcntl(fds[1], F_GETFD) | FD_CLOEXEC);

	/* One-way by construction, not merely by convention: a socketpair end is
	 * full-duplex, and the follower is a de-privileged process
	 * (fpm_http_gateway_drop_privileges()) that would otherwise be able to
	 * sendmsg() back — including SCM_RIGHTS. The master never reads this
	 * channel, so such datagrams would sit in its receive buffer for the
	 * lifetime of the process, pinning kernel memory and in-flight file
	 * references the AF_UNIX garbage collector has to walk. Bounded by SO_RCVBUF,
	 * so not a denial of service; closed here because it costs two calls. */
	shutdown(fds[0], SHUT_RD);
	shutdown(fds[1], SHUT_WR);

	ch = malloc(sizeof(*ch));
	if (!ch) {
		zlog(ZLOG_ERROR, "unable to malloc the error_log follow channel");
		close(fds[0]);
		close(fds[1]);
		return NULL;
	}

	ch->fd_master = fds[0];
	ch->fd_child = fds[1];
	ch->next = followers;
	followers = ch;

	return ch;
}
/* }}} */

void fpm_error_log_follow_parent(struct fpm_error_log_follow_s *ch) /* {{{ */
{
	if (!ch || ch->fd_child < 0) {
		return;
	}

	close(ch->fd_child);
	ch->fd_child = -1;
}
/* }}} */

void fpm_error_log_follow_child(struct fpm_error_log_follow_s *ch) /* {{{ */
{
	struct fpm_error_log_follow_s *cur = followers, *next;

	/* Every channel of every process forked before this one is in this list
	 * too, copied by fork(). Their master ends are the master's business
	 * alone: holding them open here would keep a channel alive after the
	 * master closed it, and hand this process the ability to consume another
	 * process's notification. */
	while (cur) {
		next = cur->next;
		if (cur == ch) {
			close(cur->fd_master);
			fpm_error_log_follow_fd = cur->fd_child;
		} else {
			fpm_error_log_follow_close(cur);
		}
		free(cur);
		cur = next;
	}
	followers = NULL;
}
/* }}} */

int fpm_error_log_follow_child_fd(void) /* {{{ */
{
	return fpm_error_log_follow_fd;
}
/* }}} */

void fpm_error_log_follow_child_adopt(void) /* {{{ */
{
	int adopted = 0;

	if (fpm_error_log_follow_fd < 0) {
		return;
	}

	while (adopted < FPM_ERROR_LOG_FOLLOW_BATCH) {
		union {
			struct cmsghdr align;		/* the buffer has to be aligned for a struct cmsghdr */
			char buf[CMSG_SPACE(sizeof(int))];
		} control;
		struct msghdr msg;
		struct iovec iov;
		struct cmsghdr *cmsg;
		char payload;
		ssize_t got;
		int fd;

		memset(&msg, 0, sizeof(msg));
		memset(&control, 0, sizeof(control));
		iov.iov_base = &payload;
		iov.iov_len = sizeof(payload);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = control.buf;
		msg.msg_controllen = sizeof(control.buf);

		/* MSG_CMSG_CLOEXEC so the received descriptor is never exec()able,
		 * not even for the few lines it lives before dup2() and close(). A
		 * gateway does not exec(), so this closes a reasoning gap rather than
		 * a live hole. */
		got = recvmsg(fpm_error_log_follow_fd, &msg, FPM_ERROR_LOG_FOLLOW_RECV_FLAGS);
		if (got < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (!fpm_error_log_follow_would_block(errno)) {
				zlog(ZLOG_SYSERROR, "error_log follow channel: recvmsg() failed");
			}
			return;			/* nothing (more) waiting */
		}
		if (got == 0) {
			return;			/* the master is gone; this process is next */
		}
		adopted++;

		cmsg = CMSG_FIRSTHDR(&msg);
		if (!cmsg || cmsg->cmsg_len != CMSG_LEN(sizeof(int)) ||
				cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
			/* MSG_CTRUNC lands here too: without a descriptor there is
			 * nothing to adopt and nothing to leak. */
			zlog(ZLOG_WARNING, "error_log follow channel: a notification carried no descriptor, "
				"still writing into the pre-rotation error_log");
			continue;
		}

		memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));

		/* The one thing that has to happen: the number zlog.c writes to —
		 * fpm_globals.error_log_fd, captured into its static zlog_fd — now
		 * names the new file. dup2() closes what was there, which is this
		 * process's copy of the pre-rotation file. */
		if (fd != fpm_globals.error_log_fd) {
			if (0 > dup2(fd, fpm_globals.error_log_fd)) {
				zlog(ZLOG_SYSERROR, "error_log follow channel: dup2() onto fd %d failed",
					fpm_globals.error_log_fd);
				close(fd);
				continue;
			}
			close(fd);
		}
		/* The equality case cannot happen today — this process never closes
		 * fpm_globals.error_log_fd, so the kernel cannot hand the received copy
		 * back at that number. Guarded anyway because the unguarded form is a
		 * trap: dup2(fd, fd) succeeds as a no-op and the close() that follows
		 * would then shut the log this process just adopted. */

		/* dup2() clears FD_CLOEXEC on the new descriptor; the master sets it
		 * on every error_log fd it opens (fpm_stdio_open_error_log()), so
		 * restore it rather than silently handing the log to whatever this
		 * process exec()s. */
		if (0 > fcntl(fpm_globals.error_log_fd, F_SETFD,
				fcntl(fpm_globals.error_log_fd, F_GETFD) | FD_CLOEXEC)) {
			zlog(ZLOG_WARNING, "failed to change attribute of error_log");
		}
	}
}
/* }}} */

void fpm_error_log_follow_publish(void) /* {{{ */
{
	struct fpm_error_log_follow_s *ch;

	if (fpm_globals.error_log_fd <= 0) {
		return;
	}

	for (ch = followers; ch; ch = ch->next) {
		union {
			struct cmsghdr align;
			char buf[CMSG_SPACE(sizeof(int))];
		} control;
		struct msghdr msg;
		struct iovec iov;
		struct cmsghdr *cmsg;
		int fd = fpm_globals.error_log_fd;
		ssize_t sent;

		memset(&msg, 0, sizeof(msg));
		memset(&control, 0, sizeof(control));
		iov.iov_base = (void *) &fpm_error_log_follow_byte;
		iov.iov_len = sizeof(fpm_error_log_follow_byte);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = control.buf;
		msg.msg_controllen = CMSG_SPACE(sizeof(int));

		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

		do {
			sent = sendmsg(ch->fd_master, &msg, 0);
		} while (sent < 0 && errno == EINTR);

		if (sent < 0) {
			/* Reported, not retried: the follower keeps writing into the
			 * previous file, which is a missing-lines problem, not a
			 * correctness one, and the master must not spin here. */
			zlog(ZLOG_SYSERROR, "failed to hand the reopened error_log to a forked process; "
				"it will keep writing into the previous file");
		}
	}
}
/* }}} */

void fpm_error_log_follow_free(struct fpm_error_log_follow_s *ch) /* {{{ */
{
	struct fpm_error_log_follow_s **cur = &followers;

	if (!ch) {
		return;
	}

	while (*cur) {
		if (*cur == ch) {
			*cur = ch->next;
			fpm_error_log_follow_close(ch);
			free(ch);
			return;
		}
		cur = &(*cur)->next;
	}
}
/* }}} */
