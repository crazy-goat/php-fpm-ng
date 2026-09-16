/* fpm-ng: see fpm_pool_output_log.h. */

#include "fpm_config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "fpm_pool_output_log.h"
#include "zlog.h"

int fpm_pool_output_log_redirect(const char *pool_name, const char *path) /* {{{ */
{
	int fd;

	if (!path || !*path) {
		return 0;
	}

	/* No O_TRUNC: this is the same append-only contract as cron.log
	 * (fpm_pool_cron_log_run()) -- a respawned process (cron: every run; supervisor:
	 * every restart) must not erase what the previous process already wrote. */
	fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
	if (fd < 0) {
		zlog(ZLOG_WARNING, "[pool %s] output_log: cannot open '%s' (%s), stdout/stderr left unchanged",
			pool_name, path, strerror(errno));
		return -1;
	}

	if (0 > dup2(fd, STDOUT_FILENO) || 0 > dup2(fd, STDERR_FILENO)) {
		zlog(ZLOG_WARNING, "[pool %s] output_log: cannot redirect stdout/stderr to '%s' (%s)",
			pool_name, path, strerror(errno));
		close(fd);
		return -1;
	}

	/* dup2() above installed a COPY of fd at STDOUT_FILENO/STDERR_FILENO;
	 * closing the original here does not affect either copy. Leaving it open
	 * would leak one fd for the remaining lifetime of this process (cron: the
	 * rest of that one run; supervisor: every iteration up to the next
	 * restart). */
	close(fd);
	return 0;
}
/* }}} */
