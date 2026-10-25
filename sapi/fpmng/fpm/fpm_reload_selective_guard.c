/* fpm-ng: issue #829 -- see fpm_reload_selective_guard.h for the design. */

#include "fpm_config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fpm_env.h"
#include "fpm_reload_selective.h"
#include "fpm_reload_selective_guard.h"
#include "fpm.h"
#include "zlog.h"

/* "<fd>/<st_ino>" of the pipe's write end, as the old master leaves it for the
 * new one. The inode is checked before a descriptor is used: a number alone is
 * not proof that it is still the pipe. Read once, by
 * fpm_reload_selective_guard_init_main(), in the new master. */
#define FPM_RELOAD_SELECTIVE_GUARD_ENV "FPMNG_SELECTIVE_RELOAD_GUARD"

/* Upper bound for the descriptor sweep in the guard. A larger limit is not
 * closed past this point: the guard then keeps descriptors above 65536. */
#define FPM_RELOAD_SELECTIVE_GUARD_MAX_FD 65536

/* The write end of the pipe in this process, read from the environment by
 * fpm_reload_selective_guard_init_main() before any fork. -1 when there is none.
 * The children read this, not the environment: a child clears the environment
 * (clear_env, fpm_env_init_child()) before the child hook runs. */
static int fpm_reload_selective_guard_fd = -1;
static unsigned long fpm_reload_selective_guard_ino;

/* Parses "<fd>/<st_ino>". Returns 0 when the value is not of that form. */
static int fpm_reload_selective_guard_parse(const char *value, int *fd, unsigned long *ino) /* {{{ */
{
	char *end;
	long n;

	n = strtol(value, &end, 10);
	if (end == value || *end != '/' || n < 3 || n > 65536) {
		return 0;
	}
	*fd = (int) n;
	*ino = strtoul(end + 1, &end, 10);
	return *end == '\0';
}
/* }}} */

/* Returns 1 when `fd` is still the pipe whose inode is `ino`. */
static int fpm_reload_selective_guard_fd_is_pipe(int fd, unsigned long ino) /* {{{ */
{
	struct stat st;

	if (fstat(fd, &st) < 0) {
		return 0;
	}
	return S_ISFIFO(st.st_mode) && (unsigned long) st.st_ino == ino;
}
/* }}} */

/* The guard process. Never returns: every path ends in _exit(), so no atexit()
 * handler, no cleanup and no buffered stdio of the master runs in it. */
static void fpm_reload_selective_guard_main(int rfd, int wfd, pid_t master) /* {{{ */
{
	static const int signals[] = { SIGTERM, SIGINT, SIGQUIT, SIGUSR1, SIGUSR2, SIGCHLD };
	char title[80];
	long max_fd;
	long fd;
	sigset_t none;
	unsigned int i;
	int killed;

	/* The guard must not keep the write end: its EOF is the sign that the master is gone. */
	close(wfd);

	/* The master's handlers would act on the master's state. The mask was
	 * blocked by fpm_pctl_exec() before the fork. */
	for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		signal(signals[i], SIG_DFL);
	}
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);

	snprintf(title, sizeof(title), "selective reload guard of master %d", (int) master);
	fpm_env_setproctitle(title);

	/* The guard inherits every descriptor of the master, the listening sockets
	 * included. Kept open, a listener would hold the port after the master died,
	 * which is the bug this guard exists for. Only the pipe and the error log
	 * (the WARNING below) stay. */
	max_fd = sysconf(_SC_OPEN_MAX);
	if (max_fd < 0 || max_fd > FPM_RELOAD_SELECTIVE_GUARD_MAX_FD) {
		max_fd = FPM_RELOAD_SELECTIVE_GUARD_MAX_FD;
	}
	for (fd = 3; fd < max_fd; fd++) {
		if (fd != rfd && fd != fpm_globals.error_log_fd) {
			close((int) fd);
		}
	}

	for (;;) {
		char byte;
		ssize_t got = read(rfd, &byte, 1);

		if (got == 1) {
			/* The new master adopted or discarded every entry. Nothing to do. */
			_exit(0);
		}
		if (got == 0) {
			break; /* EOF: every holder of the write end is gone */
		}
		if (errno != EINTR) {
			/* A read error must never look like the master's death. */
			_exit(0);
		}
	}

	killed = fpm_reload_selective_sigterm_carried();
	if (killed > 0) {
		zlog(ZLOG_WARNING, "issue #829: the master that a selective reload started exited before it "
						   "finished adopting the spared workers; sent SIGTERM to %d child(ren) so they do not "
						   "keep their listening sockets",
				killed);
	}
	_exit(0);
}
/* }}} */

void fpm_reload_selective_guard_init_main(void) /* {{{ */
{
	const char *value = getenv(FPM_RELOAD_SELECTIVE_GUARD_ENV);
	int fd;
	unsigned long ino;

	if (!value) {
		return;
	}

	/* Dropped here, so that no script and no child sees the variable. */
	unsetenv(FPM_RELOAD_SELECTIVE_GUARD_ENV);
	if (fpm_reload_selective_guard_parse(value, &fd, &ino) && fpm_reload_selective_guard_fd_is_pipe(fd, ino)) {
		fpm_reload_selective_guard_fd = fd;
		fpm_reload_selective_guard_ino = ino;
	}
}
/* }}} */

void fpm_reload_selective_guard_arm(void) /* {{{ */
{
	int pipefd[2];
	struct stat st;
	char value[64];
	pid_t master, pid, waited;
	int status;

	unsetenv(FPM_RELOAD_SELECTIVE_GUARD_ENV);
	if (!fpm_reload_selective_has_carried()) {
		return;
	}

	if (pipe(pipefd) < 0) {
		zlog(ZLOG_SYSERROR, "issue #829: cannot create the pipe of the selective reload guard; "
							"continuing without a guard");
		return;
	}

	/* Close-on-exec on the read end only: the write end must cross execvp(). */
	if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) < 0 || fstat(pipefd[1], &st) < 0) {
		zlog(ZLOG_SYSERROR, "issue #829: cannot prepare the pipe of the selective reload guard; "
							"continuing without a guard");
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	snprintf(value, sizeof(value), "%d/%lu", pipefd[1], (unsigned long) st.st_ino);

	/* Set before the fork: the guard copies the environment, and it needs the
	 * list the master carries. If setenv() failed after the fork, closing the
	 * write end would fire the guard on a normal reload. */
	if (setenv(FPM_RELOAD_SELECTIVE_GUARD_ENV, value, 1) != 0) {
		zlog(ZLOG_SYSERROR, "issue #829: cannot set %s; continuing without a guard",
				FPM_RELOAD_SELECTIVE_GUARD_ENV);
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}

	master = getpid();
	pid = fork();
	if (pid == 0) {
		/* The intermediate process only forks the guard and exits at once, so
		 * the master reaps it and the guard is not its child. */
		pid_t grandchild = fork();

		if (grandchild == 0) {
			fpm_reload_selective_guard_main(pipefd[0], pipefd[1], master);
		}
		_exit(grandchild < 0 ? 1 : 0);
	}

	close(pipefd[0]);

	if (pid < 0) {
		zlog(ZLOG_SYSERROR, "issue #829: cannot fork the selective reload guard; continuing without a guard");
		close(pipefd[1]);
		unsetenv(FPM_RELOAD_SELECTIVE_GUARD_ENV);
		return;
	}

	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);

	if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		zlog(ZLOG_WARNING, "issue #829: the selective reload guard did not start; continuing without a guard");
		close(pipefd[1]);
		unsetenv(FPM_RELOAD_SELECTIVE_GUARD_ENV);
		return;
	}

	/* The write end stays open here, and crosses execvp() and the daemonize
	 * fork. The new master releases it: fpm_reload_selective_guard_release(). */
}
/* }}} */

void fpm_reload_selective_guard_release(void) /* {{{ */
{
	int fd = fpm_reload_selective_guard_fd;
	char byte = '1';
	ssize_t written;

	if (fd < 0) {
		return;
	}

	/* Forgotten first: no later code of this process may use the descriptor again. */
	fpm_reload_selective_guard_fd = -1;
	if (!fpm_reload_selective_guard_fd_is_pipe(fd, fpm_reload_selective_guard_ino)) {
		return;
	}

	do {
		written = write(fd, &byte, 1);
	} while (written < 0 && errno == EINTR);
	/* EPIPE: the guard is gone already. SIGPIPE is ignored by the master, so it does not kill us. */
	close(fd);
}
/* }}} */

void fpm_reload_selective_guard_child_init(void) /* {{{ */
{
	int fd = fpm_reload_selective_guard_fd;

	if (fd < 0) {
		return;
	}

	fpm_reload_selective_guard_fd = -1;
	if (fpm_reload_selective_guard_fd_is_pipe(fd, fpm_reload_selective_guard_ino)) {
		close(fd);
	}
}
/* }}} */
