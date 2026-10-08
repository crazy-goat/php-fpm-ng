/* fpm-ng: issue #643 -- native sd_notify(3) for a systemd Type=notify unit.
 * See fpm_sd_notify.h for the contract (what READY means, and the no-op rule).
 *
 * The protocol is sd_notify(3): one datagram per message to the AF_UNIX socket
 * named by NOTIFY_SOCKET, with newline-separated KEY=VALUE pairs. A name that
 * starts with '@' is in the abstract namespace (unix(7)), which is how systemd
 * names some of its sockets. The socket is opened for each message and closed
 * again. Messages are rare (one READY per start, one RELOADING and one READY
 * per reload, one STOPPING, and one WATCHDOG=1 per period), so a cached
 * descriptor would only be state to get wrong across the execvp() of a reload.
 *
 * NOTIFY_SOCKET is deliberately NOT unset after it is read. A SIGUSR2 reload is
 * an execvp() of the master (fpm_process_ctl.c, fpm_pctl_exec()), and the next
 * generation reads the variable again to send its own READY=1. Unsetting it
 * here would turn every reload into a RELOADING=1 that is never followed by
 * READY=1, and systemd would wait for TimeoutStartSec. The price is that the
 * PHP workers see NOTIFY_SOCKET in their environment. They never send to it
 * (the is_child guard below), and systemd would drop their datagrams anyway,
 * because the unit has NotifyAccess=main.
 *
 * Only the master sends. A notification from another process is not from the
 * main pid and is dropped by NotifyAccess=main, and a second writer for one
 * unit would make the state it reports depend on which process wrote last.
 *
 * The socket is non-blocking. A full receive queue must not stall the master's
 * event loop: the message is dropped and the failure is logged.
 *
 * A failed send never stops the master. The first failure is logged. Later
 * failures are not, because the watchdog would repeat the same line every few
 * seconds for as long as the unit runs. The one-time flag is per process; a
 * reload starts a process with the flag clear, so the next failure is logged
 * again, which is the intended trade-off.
 *
 * READY=1 is not sent from the -t configuration test (fpm.c returns from
 * fpm_init() before fpm_run()), so a reload check never tells systemd that a
 * generation is up. */

#include "fpm_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "fpm.h"
#include "fpm_events.h"
#include "fpm_sd_notify.h"
#include "zlog.h"

static struct fpm_event_s watchdog_event;
static int send_failure_logged = 0;

static void fpm_sd_notify_failed(const char *path, int err) /* {{{ */
{
	if (send_failure_logged) {
		return;
	}
	send_failure_logged = 1;

	zlog(ZLOG_WARNING, "sd_notify: cannot send to NOTIFY_SOCKET '%s': %s (later failures are not logged)",
			path, strerror(err));
}
/* }}} */

/* Sends one datagram. `payload` is complete, including its trailing newline.
 * Returns 0 when it was sent or there is nothing to send to, -1 on failure. */
static int fpm_sd_notify_send(const char *payload) /* {{{ */
{
	const char *path = getenv("NOTIFY_SOCKET");
	struct sockaddr_un addr;
	socklen_t addrlen;
	size_t len;
	int fd, res, err;

	if (path == NULL || *path == '\0' || fpm_globals.is_child) {
		return 0;
	}

	len = strlen(path);
	if (len >= sizeof(addr.sun_path)) {
		fpm_sd_notify_failed(path, ENAMETOOLONG);
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (path[0] == '@') {
		/* The leading NUL byte of an abstract name is part of the name, and
		 * the address length counts it, so the length is the path length. */
		memcpy(addr.sun_path + 1, path + 1, len - 1);
		addrlen = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + len);
	} else {
		memcpy(addr.sun_path, path, len);
		addrlen = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + len + 1);
	}

	fd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (fd < 0) {
		fpm_sd_notify_failed(path, errno);
		return -1;
	}

	/* Not SOCK_CLOEXEC and SOCK_NONBLOCK: those are Linux-only, and this file
	 * also builds for the macOS development flow (build/prepare.sh). */
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
		err = errno;
		close(fd);
		fpm_sd_notify_failed(path, err);
		return -1;
	}

	res = sendto(fd, payload, strlen(payload), 0, (struct sockaddr *) &addr, addrlen);
	err = errno;
	close(fd);

	if (res < 0) {
		fpm_sd_notify_failed(path, err);
		return -1;
	}
	return 0;
}
/* }}} */

void fpm_sd_notify_ready(void) /* {{{ */
{
	fpm_sd_notify_send("READY=1\n");
}
/* }}} */

void fpm_sd_notify_reloading(void) /* {{{ */
{
	struct timespec ts;
	char payload[80];
	unsigned long long usec;

	/* MONOTONIC_USEC lets systemd date the reload from the moment it was
	 * requested, not from when the message arrived. Taken from the real
	 * monotonic clock, not fpm_clock, because systemd compares it with its own
	 * clock and the debug clock (FPMNG_DEBUG_CLOCK) is a virtual one. */
	clock_gettime(CLOCK_MONOTONIC, &ts);
	usec = (unsigned long long) ts.tv_sec * 1000000ULL + (unsigned long long) ts.tv_nsec / 1000ULL;
	snprintf(payload, sizeof(payload), "RELOADING=1\nMONOTONIC_USEC=%llu\n", usec);

	fpm_sd_notify_send(payload);
}
/* }}} */

void fpm_sd_notify_stopping(void) /* {{{ */
{
	fpm_sd_notify_send("STOPPING=1\n");
}
/* }}} */

static void fpm_sd_notify_watchdog_tick(struct fpm_event_s *ev, short which, void *arg) /* {{{ */
{
	(void) ev;
	(void) which;
	(void) arg;

	fpm_sd_notify_send("WATCHDOG=1\n");
}
/* }}} */

void fpm_sd_notify_watchdog_start(void) /* {{{ */
{
	const char *usec_text = getenv("WATCHDOG_USEC");
	const char *pid_text = getenv("WATCHDOG_PID");
	char *end = NULL;
	unsigned long long usec;
	unsigned long period;

	if (usec_text == NULL || *usec_text == '\0' || fpm_globals.is_child) {
		return;
	}

	/* systemd sets WATCHDOG_PID for a process it wants to ping the unit.
	 * Another process that inherited the variable must not ping (the rule of
	 * sd_watchdog_enabled(3)). */
	if (pid_text != NULL && *pid_text != '\0' && strtol(pid_text, NULL, 10) != (long) getpid()) {
		return;
	}

	errno = 0;
	usec = strtoull(usec_text, &end, 10);
	if (errno != 0 || end == usec_text || *end != '\0' || usec == 0) {
		zlog(ZLOG_WARNING, "sd_notify: WATCHDOG_USEC='%s' is not a positive number, the watchdog is off",
				usec_text);
		return;
	}

	/* Half of the timeout, as sd_watchdog_enabled(3) recommends, so that one
	 * late ping does not kill the service. Microseconds to milliseconds, and
	 * never a zero period, which would turn the event loop into a busy loop. */
	period = (unsigned long) (usec / 2000ULL);
	if (period == 0) {
		period = 1;
	}

	fpm_event_set_timer(&watchdog_event, FPM_EV_PERSIST, &fpm_sd_notify_watchdog_tick, NULL);
	if (0 > fpm_event_add(&watchdog_event, period)) {
		zlog(ZLOG_WARNING, "sd_notify: cannot schedule WATCHDOG=1, the watchdog is off");
		return;
	}

	/* The first ping at once: systemd counts from the start of the unit, and
	 * READY=1 goes out right after this call. */
	fpm_sd_notify_send("WATCHDOG=1\n");
}
/* }}} */
