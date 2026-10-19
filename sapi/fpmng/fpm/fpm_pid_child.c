/* fpm-ng: issue #891 -- see fpm_pid_child.h.
 *
 * A pid that a reload carries across execvp() (FPMNG_RELOAD_SURVIVORS,
 * FPMNG_SELECTIVE_RELOAD_SURVIVORS) can have been reaped in between, and the
 * kernel can then give it to an unrelated process. kill(pid, 0) cannot tell
 * the two apart: it succeeds for any live process.
 *
 * Parentage cannot tell them apart either. With daemonize = yes the exec'd
 * image forks again (third_party/php-src/sapi/fpm/fpm/fpm_unix.c:618), so the
 * new master is not the parent of the pids it was carried. A process start
 * time stays the same for the whole life of a process, and a reused pid has
 * another one. Linux shows it in /proc/<pid>/stat, field 22.
 *
 * Without /proc (macOS, or a Linux without it) no start time can be read. The
 * start is then 0, and fpm_pid_is_same() falls back to kill(pid, 0), which is
 * the behaviour before #891. A reused pid passes that check there. Not
 * measured on macOS. */

#include "fpm_config.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "fpm_pid_child.h"

unsigned long long fpm_pid_start_time(pid_t pid) /* {{{ */
{
#ifdef __linux__
	char path[64], buf[1024], *p;
	unsigned long long start;
	int fd, field = 3;
	ssize_t n;

	snprintf(path, sizeof(path), "/proc/%ld/stat", (long) pid);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return 0;
	}
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) {
		return 0;
	}
	buf[n] = '\0';

	/* The command name (field 2) is in parentheses and may hold spaces or
	 * ')', so the fields are counted after the last ')'. Field 3 is the first
	 * one there, and field 22 is the twentieth. */
	p = strrchr(buf, ')');
	if (!p) {
		return 0;
	}
	p++;
	while (field < 22) {
		while (*p == ' ') {
			p++;
		}
		if (!*p) {
			return 0;
		}
		while (*p && *p != ' ') {
			p++;
		}
		field++;
	}
	start = strtoull(p, NULL, 10);

	return start;
#else
	(void) pid;
	return 0;
#endif
}
/* }}} */

int fpm_pid_is_same(pid_t pid, unsigned long long start) /* {{{ */
{
	if (start == 0) {
		return kill(pid, 0) == 0;
	}

	return fpm_pid_start_time(pid) == start;
}
/* }}} */
