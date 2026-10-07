/* fpm-ng: issue #640 -- the configuration gate in front of a SIGUSR2 reload.
 * See fpm_reload_config_check.h for why this exists, why it runs in a child
 * that execs the binary, and why "the check could not be run" is not a
 * refusal. */

#include "fpm_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fpm.h"
#include "fpm_reload_config_check.h"
#include "zlog.h"

/* The one flag that turns an invocation into a configuration test: fpm_main.c
 * counts it (test_conf++, third_party/php-src/sapi/fpm/fpm/fpm_main.c:1654) and
 * fpm_conf_init_main() validates and then returns -1 with test_successful set,
 * which fpm_init() turns into FPM_INIT_EXIT_OK (fpm.c:93). Exit status 0 is
 * therefore "the configuration loaded", and nothing else is. */
static char FPM_CONFIG_TEST_FLAG[] = "-t";

/* The child half. Between fork() and execvp() nothing of the master's may be
 * called: this process is a copy of the master, and if execvp() fails the only
 * thing it has to do is report why and leave. The reason travels through a pipe
 * rather than through a zlog() call, because zlog's output can be buffered
 * (log_buffering) and _exit() does not flush it -- a lost "cannot exec" would
 * leave the master with a check that failed silently. */
static void fpm_reload_config_check_child(char **argv, int report_fd) /* {{{ */
{
	int err;
	ssize_t ignored;

	execvp(argv[0], argv);

	err = errno;
	ignored = write(report_fd, &err, sizeof(err));
	(void) ignored;

	_exit(127);
}
/* }}} */

int fpm_reload_config_check(int argc, const char *const *argv) /* {{{ */
{
	char **checked;
	int report[2] = { -1, -1 };
	int exec_errno = 0;
	ssize_t reported;
	pid_t pid;
	int status = 0;
	int waited;

	/* Not reachable in a running master (fpm_pctl_init_main() refuses to start
	 * without a saved argv), and not a refusal either: there is nothing to test
	 * and no evidence of anything being wrong. */
	if (argc < 1 || !argv || !argv[0] || !*argv[0]) {
		zlog(ZLOG_WARNING, "reload: this master saved no argv, so the new configuration cannot be "
						   "checked; reloading without a check");
		return 1;
	}

	/* What the child execs: this master's own arguments plus `-t`. APPENDED,
	 * never substituted -- the point of the check is to load exactly what the
	 * next generation is about to load, php.ini and -c/-d/-O/-F included. A
	 * `-y fd:N` master cannot exist anyway: fpm_conf.c refuses a descriptor
	 * config without -t, because a reload could not read it again. */
	checked = malloc(((size_t) argc + 2) * sizeof(*checked));

	if (!checked) {
		zlog(ZLOG_WARNING, "reload: out of memory for the configuration check; "
						   "reloading without a check");
		return 1;
	}

	memcpy(checked, argv, (size_t) argc * sizeof(*checked));
	checked[argc] = FPM_CONFIG_TEST_FLAG;
	checked[argc + 1] = NULL;

	if (pipe(report) < 0) {
		zlog(ZLOG_SYSERROR, "reload: cannot create the pipe the configuration check reports through; "
							"reloading without a check");
		free(checked);
		return 1;
	}

	/* Close-on-exec on the write end, and before the fork: if execvp() succeeds
	 * the descriptor goes away with the old image, so the read() below sees EOF
	 * instead of waiting forever for a report that is never coming. */
	if (fcntl(report[1], F_SETFD, fcntl(report[1], F_GETFD) | FD_CLOEXEC) < 0) {
		zlog(ZLOG_SYSERROR, "reload: cannot set close-on-exec on the configuration check's pipe; "
							"reloading without a check");
		close(report[0]);
		close(report[1]);
		free(checked);
		return 1;
	}

	pid = fork();

	if (pid < 0) {
		zlog(ZLOG_SYSERROR, "reload: cannot fork the configuration check; reloading without a check");
		close(report[0]);
		close(report[1]);
		free(checked);
		return 1;
	}

	if (pid == 0) {
		close(report[0]);
		fpm_reload_config_check_child(checked, report[1]);
		/* not reached */
	}

	close(report[1]);

	/* Waiting for THIS pid, never waitpid(-1): the master's own SIGCHLD path
	 * (fpm_children_bury()) reaps whatever it can find, and it must never find
	 * this one -- or the exit of an ordinary worker in the meantime would be
	 * left as a zombie until the next pass. EINTR is the signal handler in
	 * fpm_signals.c doing its job, and the check is still running, so keep
	 * waiting. The event loop is blocked for the duration, which costs one
	 * short-lived process; a SIGTERM or SIGQUIT arriving meanwhile is read and
	 * acted on right after, exactly as it would have been without the gate. */
	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);

	if (waited < 0) {
		zlog(ZLOG_SYSERROR, "reload: cannot wait for the configuration check; reloading without a check");
		close(report[0]);
		free(checked);
		return 1;
	}

	/* The child is gone, so this is either the one errno it wrote or EOF
	 * because it exec'd (which is the point) or died before writing anything. */
	reported = read(report[0], &exec_errno, sizeof(exec_errno));
	close(report[0]);
	free(checked);

	if (reported == (ssize_t) sizeof(exec_errno)) {
		/* The check could not run at all. This is also what happens when the
		 * binary this master was started from is gone: the reload's own
		 * execvp() would fail the same way, and fpm_pctl_exec() logs that and
		 * cleans up after itself (issue #690). Refusing here instead would keep
		 * the running generation serving a binary the operator can no longer
		 * reload, with nothing but a warning to explain it. */
		zlog(ZLOG_WARNING, "reload: cannot run the configuration check: execvp(\"%s\") failed: %s; "
						   "reloading without a check",
				argv[0], strerror(exec_errno));
		return 1;
	}

	if (WIFSIGNALED(status)) {
		zlog(ZLOG_WARNING, "reload: the configuration check was killed by signal %d; "
						   "reloading without a check",
				WTERMSIG(status));
		return 1;
	}

	if (!WIFEXITED(status)) {
		zlog(ZLOG_WARNING, "reload: the configuration check did not exit normally; "
						   "reloading without a check");
		return 1;
	}

	if (WEXITSTATUS(status) == FPM_EXIT_OK) {
		return 1;
	}

	/* The refusal. The child's own diagnostics -- which directive, which pool,
	 * which line -- are in the log this configuration names (the header says
	 * where), and they are above this line: the child wrote them before it
	 * exited, and the waitpid() above is what put them there first. */
	zlog(ZLOG_ERROR, "reload refused: the configuration test says %s is not valid (exit status %d), "
					 "so the pools that are running keep serving the configuration they were started with; "
					 "fix the configuration and send SIGUSR2 again",
			fpm_globals.config ? fpm_globals.config : "the configuration file", WEXITSTATUS(status));
	return 0;
}
/* }}} */
