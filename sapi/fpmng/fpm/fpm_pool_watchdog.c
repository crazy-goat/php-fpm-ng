/* fpm-ng: shared pidfd watchdog. See fpm_pool_watchdog.h. Extracted from
 * fpm_pool_supervisor.c (originally written for supervisor.stop_timeout) when
 * pool.type = cron needed exactly the same mechanism for cron.timeout.
 */

#include "fpm_config.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
/* Syscall numbers not always exposed by libc headers (musl/older glibc) —
 * unchanged for years on x86_64/aarch64. */
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#endif

#include "fpm_pool_watchdog.h"

static int fpm_pool_watchdog_pidfd_open(pid_t pid) /* {{{ */
{
#if defined(__linux__) && defined(SYS_pidfd_open)
	return (int) syscall(SYS_pidfd_open, pid, 0);
#else
	(void) pid;
	return -1;
#endif
}
/* }}} */

static int fpm_pool_watchdog_pidfd_send_signal(int pidfd, int sig) /* {{{ */
{
#if defined(__linux__) && defined(SYS_pidfd_send_signal)
	return (int) syscall(SYS_pidfd_send_signal, pidfd, sig, NULL, 0);
#else
	(void) pidfd;
	(void) sig;
	return -1;
#endif
}
/* }}} */

pid_t fpm_pool_watchdog_arm(pid_t target_pid, unsigned timeout_seconds) /* {{{ */
{
	int pidfd;
	pid_t watchdog;

	/* Open the pidfd for the target BEFORE forking — see fpm_pool_watchdog.h and
	 * NOTES.md 3o for the full rationale (including why this is safe even when
	 * called from a signal handler). */
	pidfd = fpm_pool_watchdog_pidfd_open(target_pid);
	watchdog = fork();

	if (watchdog == 0) {
		if (pidfd >= 0) {
			struct pollfd pfd;

			pfd.fd = pidfd;
			pfd.events = POLLIN;
			pfd.revents = 0;

			if (poll(&pfd, 1, (int) timeout_seconds * 1000) == 0) {
				/* timeout, not POLLIN: the target is still alive */
				fpm_pool_watchdog_pidfd_send_signal(pidfd, SIGKILL);
			}
			/* POLLIN: cel juz sam sie zakonczyl, nic do roboty */
		} else {
			/* Fallback without pidfd (non-Linux — local builds/tests only, never
			 * the target platform): check once per second instead of blindly sleeping
			 * for the whole timeout, so we do not leave a visible "tail" after a
			 * target that finished by itself after a fraction of a second. The narrow
			 * PID-reuse race window between kill(pid,0) and kill(pid,SIGKILL) is
			 * documented. */
			int remaining = (int) timeout_seconds;

			while (remaining > 0) {
				if (kill(target_pid, 0) != 0) {
					break;
				}
				sleep(1);
				remaining--;
			}
			if (kill(target_pid, 0) == 0) {
				kill(target_pid, SIGKILL);
			}
		}
		_exit(0);
	}

	if (pidfd >= 0) {
		close(pidfd);
	}

	return watchdog;
}
/* }}} */
