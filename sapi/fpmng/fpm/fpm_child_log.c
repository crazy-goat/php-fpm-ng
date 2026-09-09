/* fpm-ng: child -> master log channel. See fpm_child_log.h for the why. */

#include "fpm_config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "php_network.h"

#include "fpm.h"
#include "fpm_children.h"
#include "fpm_events.h"
#include "fpm_sockets.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_type.h"
#include "fpm_child_log.h"
#include "zlog.h"

/* One record is one datagram: a fixed header (level byte + the sending child's
 * pid) followed by the message, without a terminating newline. 4 KB of message
 * is twice zlog's own MAX_BUF_LENGTH (2048, zlog.c), so every message zlog()
 * itself can produce fits; only a zlog_stream (PHP messages, which may be
 * multi-line) can be longer, and one of those is truncated with a visible
 * "..." rather than silently cut.
 *
 * The pid travels in the record because the channel is per POOL, not per child:
 * with supervisor.processes = 4 all four children emit the same
 * "[pool sup] supervisor: script exited (code 3) ..." text, the child's own
 * zlog prefix is bypassed (the relay gets the bare message), and the master
 * re-emits under its own pid — so without this the lines are indistinguishable.
 * The catch_workers_output path this replaces did carry it ("child N said into
 * stderr"), so keeping it is parity, not a new idea. */
#define FPM_CHILD_LOG_MAX_MSG 4096
#define FPM_CHILD_LOG_HDR (1 + (int) sizeof(pid_t))

/* A record larger than the socket's datagram limit is rejected whole with
 * EMSGSIZE, and this is the logging path, so there is nowhere to report that.
 * Both ends therefore ask for a buffer with room for several maximal records
 * (Linux honours SO_SNDBUF as the datagram limit for AF_UNIX; macOS derives it
 * from net.local.dgram.maxdgram, default 2048, which one maximal record already
 * exceeds). The request is advisory — see the EMSGSIZE retry in
 * fpm_child_log_relay() for what happens when the kernel keeps a smaller
 * limit. */
#define FPM_CHILD_LOG_SOCKBUF (4 * (FPM_CHILD_LOG_MAX_MSG + FPM_CHILD_LOG_HDR))

/* Records handled per event-loop callback. The master's SIGTERM/SIGCHLD/SIGUSR1
 * arrive through a socketpair drained by this same event loop (fpm_signals.c,
 * fpm_events.c), so a callback that keeps reading for as long as a chatty child
 * keeps writing is a callback during which no child is reaped and no log is
 * reopened. Draining a bounded batch and returning costs one more trip through
 * the loop — the fd stays readable, the event is level-triggered — and cannot
 * starve anything. 64 is arbitrary but far above the handful of records a
 * script run produces. */
#define FPM_CHILD_LOG_BATCH 64

struct fpm_child_log_channel_s {
	struct fpm_worker_pool_s *wp;
	int fd_read;
	int fd_write;
	unsigned registered:1;		/* read end is in the event loop */
	struct fpm_event_s ev;
	struct fpm_child_log_channel_s *next;
};

static struct fpm_child_log_channel_s *fpm_child_log_channels = NULL;

/* Child side: the write end of OUR pool's channel, or -1 when this child's
 * pool type does not use one (every classic worker), plus the pid that goes
 * into every record. Captured once, after fork, rather than calling getpid()
 * per message. */
static int fpm_child_log_fd = -1;
static pid_t fpm_child_log_pid = 0;

static struct fpm_child_log_channel_s *fpm_child_log_channel_of(struct fpm_worker_pool_s *wp)
{
	struct fpm_child_log_channel_s *ch;

	for (ch = fpm_child_log_channels; ch; ch = ch->next) {
		if (ch->wp == wp) {
			return ch;
		}
	}

	return NULL;
}

static void fpm_child_log_said(struct fpm_event_s *ev, short which, void *arg) /* {{{ */
{
	struct fpm_child_log_channel_s *ch = (struct fpm_child_log_channel_s *) arg;
	/* One byte more than the largest record the relay can send, so that the
	 * NUL below has somewhere to go: recv() on SOCK_DGRAM discards whatever
	 * does not fit, silently, and a buffer of exactly the record size would
	 * therefore eat the last byte of every maximal message. */
	char buf[FPM_CHILD_LOG_MAX_MSG + FPM_CHILD_LOG_HDR + 1];
	ssize_t got;
	int handled;

	(void) ev;
	(void) which;

	if (!ch) {
		return;
	}

	/* Loop: the event is level-triggered, but several records may already be
	 * queued (a restart logs its NOTICE while the previous run's PHP messages
	 * are still unread), and reading only one per loop iteration would let the
	 * socket buffer fill up and block the child. Bounded — see
	 * FPM_CHILD_LOG_BATCH. */
	for (handled = 0; handled < FPM_CHILD_LOG_BATCH; handled++) {
		int level;
		pid_t pid;

		got = recv(ch->fd_read, buf, sizeof(buf) - 1, 0);
		if (got < FPM_CHILD_LOG_HDR) {
			/* got == 0 cannot mean "child gone" here: the master keeps its own
			 * copy of the write end open for the pool's whole life (see
			 * fpm_child_log.h), so there is nothing to tear down — a datagram
			 * shorter than the header is simply not a record we can attribute a
			 * level and a child to. */
			if (got < 0 && !PHP_IS_TRANSIENT_ERROR(errno) && errno != EINTR) {
				zlog(ZLOG_SYSERROR, "[pool %s] unable to read the child log channel",
					ch->wp->config->name);
			}
			return;
		}

		level = buf[0] & ZLOG_LEVEL_MASK;
		if (level < ZLOG_DEBUG || level > ZLOG_ALERT) {
			/* Not something this master wrote the sending end of. Do not drop
			 * it — that would hide exactly the sort of surprise worth seeing —
			 * but do not trust the level either. */
			level = ZLOG_WARNING;
		}
		memcpy(&pid, buf + 1, sizeof(pid));
		buf[got] = '\0';

		/* The child's message already carries its own "[pool %s]" prefix, by
		 * the same convention every master-side message follows, so it is
		 * re-emitted with only the sending child appended: the resulting line
		 * is indistinguishable from one the master produced itself, which is
		 * the whole point of issue #121. The suffix, not a prefix, so that a
		 * log grep for the pool and for the message text keeps working.
		 *
		 * A record below the master's log_level is formatted by the child and
		 * dropped here, because zlog() calls an external logger BEFORE applying
		 * that filter (zlog.c, vzlog()). That wastes a datagram per filtered
		 * DEBUG line; in a supervisor/cron child, which serves no requests,
		 * the whole traffic is a handful of records per script run. */
		zlog(level, "%s (child %d)", buf + FPM_CHILD_LOG_HDR, (int) pid);
	}
}
/* }}} */

int fpm_child_log_prepare(struct fpm_child_s *child) /* {{{ */
{
	struct fpm_child_log_channel_s *ch;
	int fds[2];

	if (!fpm_pool_type_of(child->wp)->child_logs_via_master) {
		return 0;
	}

	if (fpm_child_log_channel_of(child->wp)) {
		return 0;
	}

	if (0 > socketpair(AF_UNIX, SOCK_DGRAM, 0, fds)) {
		zlog(ZLOG_SYSERROR, "[pool %s] failed to create the child log channel; "
			"this pool's own messages will not reach error_log",
			child->wp->config->name);
		return 0;
	}

	/* Read end non-blocking, because the master reads it from the event loop
	 * and must never block there. Write end deliberately left BLOCKING: a
	 * child that fills the socket buffer waits for the master instead of
	 * throwing away the message that explains why it is restarting. */
	if (0 > fd_set_blocked(fds[0], 0)) {
		zlog(ZLOG_SYSERROR, "[pool %s] failed to unblock the child log channel",
			child->wp->config->name);
		close(fds[0]);
		close(fds[1]);
		return 0;
	}

	/* Advisory, both ends, both directions: see FPM_CHILD_LOG_SOCKBUF. A kernel
	 * that refuses only costs us the EMSGSIZE retry in fpm_child_log_relay(),
	 * so the return value is deliberately not checked. */
	{
		int sockbuf = FPM_CHILD_LOG_SOCKBUF;

		(void) setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &sockbuf, sizeof(sockbuf));
		(void) setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &sockbuf, sizeof(sockbuf));
	}

	/* A supervised script may exec (proc_open, exec()); nothing it starts has
	 * any business holding the master's log channel. */
	fcntl(fds[0], F_SETFD, fcntl(fds[0], F_GETFD) | FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, fcntl(fds[1], F_GETFD) | FD_CLOEXEC);

	ch = malloc(sizeof(*ch));
	if (!ch) {
		zlog(ZLOG_ERROR, "[pool %s] unable to malloc the child log channel",
			child->wp->config->name);
		close(fds[0]);
		close(fds[1]);
		return 0;
	}

	memset(ch, 0, sizeof(*ch));
	ch->wp = child->wp;
	ch->fd_read = fds[0];
	ch->fd_write = fds[1];
	ch->next = fpm_child_log_channels;
	fpm_child_log_channels = ch;

	return 0;
}
/* }}} */

void fpm_child_log_parent_use(struct fpm_child_s *child) /* {{{ */
{
	struct fpm_child_log_channel_s *ch = fpm_child_log_channel_of(child->wp);

	if (!ch || ch->registered) {
		return;
	}

	fpm_event_set(&ch->ev, ch->fd_read, FPM_EV_READ, fpm_child_log_said, ch);
	fpm_event_add(&ch->ev, 0);
	ch->registered = 1;
}
/* }}} */

void fpm_child_log_child_use(struct fpm_child_s *child) /* {{{ */
{
	struct fpm_child_log_channel_s *ch;

	for (ch = fpm_child_log_channels; ch; ch = ch->next) {
		if (ch->wp == child->wp) {
			/* Ours: the read end belongs to the master alone — a child holding
			 * it could steal its own pool's records out of the queue. */
			close(ch->fd_read);
			ch->fd_read = -1;
			fpm_child_log_fd = ch->fd_write;
			fpm_child_log_pid = getpid();
		} else {
			close(ch->fd_read);
			close(ch->fd_write);
			ch->fd_read = -1;
			ch->fd_write = -1;
		}
	}
}
/* }}} */

/* Cut the message down to `len` bytes, marking the cut visibly. Kept separate
 * because both the size cap and the EMSGSIZE retry below need it. */
static void fpm_child_log_truncate(char *buf, size_t len) /* {{{ */
{
	if (len >= sizeof("...") - 1) {
		memcpy(buf + FPM_CHILD_LOG_HDR + len - (sizeof("...") - 1), "...",
			sizeof("...") - 1);
	}
}
/* }}} */

static void fpm_child_log_relay(int level, char *msg, size_t len) /* {{{ */
{
	char buf[FPM_CHILD_LOG_MAX_MSG + FPM_CHILD_LOG_HDR];
	int attempts;

	if (fpm_child_log_fd < 0) {
		return;
	}

	if (len > FPM_CHILD_LOG_MAX_MSG) {
		len = FPM_CHILD_LOG_MAX_MSG;
		memcpy(buf + FPM_CHILD_LOG_HDR, msg, len);
		fpm_child_log_truncate(buf, len);
	} else {
		memcpy(buf + FPM_CHILD_LOG_HDR, msg, len);
	}

	buf[0] = (char) (level & ZLOG_LEVEL_MASK);
	memcpy(buf + 1, &fpm_child_log_pid, sizeof(fpm_child_log_pid));

	/* One write() is one datagram, so a short write cannot happen and there is
	 * nothing to resume; the loop exists for two failures that are worth one
	 * more try each:
	 *
	 * - EINTR: a supervisor/cron child takes SIGTERM/SIGCHLD while parked in a
	 *   blocking write(). Retried, but a BOUNDED number of times: an unbounded
	 *   retry swallows the signal for as long as the master is slow, which
	 *   delays the child's own termination check until process_control_timeout
	 *   SIGKILLs it. A lost log line is the cheaper of the two.
	 * - EMSGSIZE: the kernel kept a datagram limit below our record despite the
	 *   SO_SNDBUF request in fpm_child_log_prepare() (macOS
	 *   net.local.dgram.maxdgram is 2048 by default, which one maximal record
	 *   exceeds). Halve the message and try again — a truncated line still says
	 *   why the pool is restarting; a dropped one does not.
	 *
	 * Any other error is unreportable by definition — this IS the logging path
	 * — so it is dropped rather than turned into recursion. */
	for (attempts = 0; attempts < 8; attempts++) {
		if (write(fpm_child_log_fd, buf, len + FPM_CHILD_LOG_HDR) >= 0) {
			return;
		}
		if (errno == EMSGSIZE && len > sizeof("...") - 1) {
			len /= 2;
			fpm_child_log_truncate(buf, len);
			continue;
		}
		if (errno != EINTR) {
			return;
		}
	}
}
/* }}} */

void fpm_child_log_init_child(struct fpm_worker_pool_s *wp) /* {{{ */
{
	int devnull;

	if (fpm_child_log_fd < 0) {
		return;
	}

	/* Install the relay first, so that the failure below has somewhere to go. */
	zlog_set_external_logger(fpm_child_log_relay);

	/* zlog() calls the external logger IN ADDITION to writing to its fd, and
	 * fpm_stdio_init_child() has just left that fd at -1, which zlog() reads as
	 * "write to STDERR_FILENO". With catch_workers_output = yes that stderr is
	 * a pipe to the master, so every message would arrive twice: once relayed
	 * at its own level, once wrapped as WARNING "child N said into stderr".
	 * Point zlog at /dev/null instead of touching STDERR, which still has to
	 * carry whatever the supervised script writes there itself.
	 *
	 * Failing to open /dev/null is not worth refusing to run the supervised
	 * process over: the only consequence is that duplicate copy, and only for
	 * a pool that asked for catch_workers_output. */
	devnull = open("/dev/null", O_WRONLY);
	if (0 > devnull) {
		/* strerror(errno) spelled out rather than ZLOG_SYSERROR: see the warning
		 * in fpm_child_log.h — the relay never sees that suffix. */
		zlog(ZLOG_ERROR, "[pool %s] failed to open /dev/null: %s (%d); this pool's "
			"own messages may be logged twice", wp->config->name, strerror(errno), errno);
		return;
	}
	fcntl(devnull, F_SETFD, fcntl(devnull, F_GETFD) | FD_CLOEXEC);
	zlog_set_fd(devnull, 0);
}
/* }}} */
