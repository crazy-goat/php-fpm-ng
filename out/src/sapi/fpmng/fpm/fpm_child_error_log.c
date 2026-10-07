/* fpm-ng: see fpm_child_error_log.h — issue #130. */

#include "fpm_config.h"

#include <stdarg.h>

#include "fpm.h"
#include "fpm_child_error_log.h"
#include "zlog.h"

/* Off in the master and in every ordinary child; set once, never cleared —
 * a process that starts writing into error_log itself never stops. */
static int fpm_child_error_log_decorates = 0;

void fpm_child_error_log_use(void) /* {{{ */
{
	/* Only when this process really holds an error_log FILE. The two cases
	 * left out are not oversights:
	 *
	 * - error_log_fd == -1: an ordinary pool child, after
	 *   fpm_stdio_init_child() closed the log; zlog() then writes to
	 *   STDERR_FILENO, which is either /dev/null or the pipe the master reads
	 *   and decorates itself (fpm_stdio_child_said()). Decorating here would
	 *   put a second timestamp inside a line the master timestamps.
	 * - error_log_fd == ZLOG_SYSLOG: zlog_buf_prefix() never looks at
	 *   is_child on the syslog path (the test is commented out upstream) and
	 *   syslogd supplies the time, so there is nothing to restore.
	 *
	 * The gate reads fpm_globals.error_log_fd, while what vzlog() writes to is
	 * zlog.c's own static zlog_fd; the two diverge when fpm_use_error_log() is
	 * false (a foreground run on a terminal), where error_log_fd is open but
	 * zlog_fd is still -1 and the line goes to the terminal. Decorating is
	 * right there too: the master writes to that same terminal with is_child
	 * clear, so both processes stay in the one format.
	 */
	if (fpm_globals.error_log_fd > 0) {
		fpm_child_error_log_decorates = 1;
	}
}
/* }}} */

void fpmng_zlog_ex(const char *function, int line, int flags, const char *fmt, ...) /* {{{ */
{
	va_list args;

	va_start(args, fmt);
	if (fpm_child_error_log_decorates) {
		/* zlog_buf_prefix() reads fpm_globals.is_child and nothing else to
		 * decide whether the line gets the timestamp and the pid. Restored
		 * immediately, because is_child answers a different question
		 * everywhere else (fpm_process_ctl.c, fpm_children.c) and the answer
		 * there stays yes.
		 *
		 * Nothing between the two assignments touches errno, so a
		 * ZLOG_SYSERROR still finds the errno it was raised with — the reason
		 * this reroutes the prefix decision instead of reformatting the
		 * message, as the child log relay has to (fpm_child_log.h).
		 *
		 * Not reentrant, and does not need to be: FPM's signal handlers write
		 * a byte into a pipe and never log (fpm_signals.c), so no other code
		 * can observe is_child inside this window. */
		int saved_is_child = fpm_globals.is_child;

		fpm_globals.is_child = 0;
		vzlog(function, line, flags, fmt, args);
		fpm_globals.is_child = saved_is_child;
	} else {
		vzlog(function, line, flags, fmt, args);
	}
	va_end(args);
}
/* }}} */
