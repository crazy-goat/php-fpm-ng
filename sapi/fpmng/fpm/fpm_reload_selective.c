/* fpm-ng: issue #330 -- see fpm_reload_selective.h for the design. */

#include "fpm_config.h"

#include <sys/types.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fpm_reload_selective.h"
#include "fpm_worker_pool.h"
#include "fpm_children.h"
#include "fpm_children_extra.h"
#include "fpm_clock.h"
#include "zlog.h"

/* One env var for every pool, "name:pid,pid,pid" groups separated by ';' --
 * the same "no turning a pool name into an env-var-safe identifier" reasoning
 * as #329's FPM_SUPERVISOR_RELOAD_SURVIVOR_ENV (fpm_pool_supervisor.c), one
 * level deeper: ',' now separates pids of the SAME pool, ';' separates
 * different pools. A pool name containing either character (or ':') cannot be
 * represented and is refused below, the same way #329 refuses one containing
 * ':' or ','. A distinct name from #329's own env var: the two mechanisms
 * are independent (this one can carry an ENTIRE pool's children; #329 carries
 * at most one, for a pool that IS restarting) and must not be able to
 * misparse each other's contents if both happen to fire on the same
 * reload -- a config with both an unchanged supervisor pool and a changed
 * one exercises exactly that. */
#define FPM_RELOAD_SELECTIVE_ENV "FPMNG_SELECTIVE_RELOAD_SURVIVORS"

static int fpm_reload_selective_name_ok(const char *name) /* {{{ */
{
	return !strpbrk(name, ":,;");
}
/* }}} */

void fpm_reload_selective_spare_pool(struct fpm_worker_pool_s *wp) /* {{{ */
{
	const char *name = wp->config->name;
	const char *existing;
	char *pids = NULL;
	size_t pids_len = 0, pids_cap = 0;
	struct fpm_child_s *child;
	int n = 0;

	if (!fpm_reload_selective_name_ok(name)) {
		zlog(ZLOG_WARNING, "[pool %s] issue #330: pool name contains ':', ',' or ';', "
			"cannot carry this pool's children across a selective reload -- "
			"reloading it normally instead", name);
		return;
	}

	if (wp->running_children == 0) {
		return; /* nothing to spare */
	}

	/* Build "pid,pid,pid" by repeatedly detaching the oldest remaining child
	 * (fpm_children_detach_oldest() already exists for #329 and does exactly
	 * the unlink-without-signalling this needs; calling it in a loop until
	 * wp->children is empty is the whole of "detach every child" -- no new
	 * detach-all primitive needed). */
	while ((child = fpm_children_detach_oldest(wp)) != NULL) {
		char one[32];
		int one_len;
		size_t needed;

		one_len = snprintf(one, sizeof(one), "%s%d", n ? "," : "", (int) child->pid);
		needed = pids_len + (size_t) one_len + 1;

		if (needed > pids_cap) {
			size_t new_cap = pids_cap ? pids_cap * 2 : 128;
			char *next;

			while (new_cap < needed) {
				new_cap *= 2;
			}
			next = realloc(pids, new_cap);
			if (!next) {
				/* Best effort, same as #329's env var helpers: this child is
				 * dropped from the handoff and simply is not adopted after
				 * execvp() -- the new generation's ordinary fork loop covers
				 * the gap for it, same as a crashed child would. */
				free(child);
				continue;
			}
			pids = next;
			pids_cap = new_cap;
		}
		memcpy(pids + pids_len, one, (size_t) one_len);
		pids_len += (size_t) one_len;
		pids[pids_len] = '\0';
		n++;

		/* This process is about to execvp() away without ever waiting on
		 * `child` -- fpm_children_free() would close fds pointlessly
		 * (fd_stdout/fd_stderr may be real pipes for a spared child, unlike
		 * #329's single-survivor case, since this is an ordinary child that
		 * HAD been forked normally by fpm_resources_prepare()). Closing them
		 * here would race the child's own use of the write end nowhere --
		 * they are read ends on the master's side and cost nothing left
		 * open across an execvp() that closes everything CLOEXEC anyway
		 * (fpm_child_cloexec() marks the listening socket; stdio pipes are
		 * not, but a leaked read-end fd in a process about to replace its
		 * own image is immediately gone regardless). Just free the struct. */
		free(child);
	}

	if (n == 0) {
		free(pids);
		return;
	}

	existing = getenv(FPM_RELOAD_SELECTIVE_ENV);
	{
		size_t len = (existing ? strlen(existing) : 0) + strlen(name) + pids_len + 3;
		char *next = malloc(len);

		if (next) {
			if (existing && *existing) {
				snprintf(next, len, "%s;%s:%s", existing, name, pids);
			} else {
				snprintf(next, len, "%s:%s", name, pids);
			}
			setenv(FPM_RELOAD_SELECTIVE_ENV, next, 1);
			free(next);
		}
	}
	free(pids);

	zlog(ZLOG_NOTICE, "[pool %s] issue #330: config unchanged -- sparing all %d running "
		"child(ren) from this reload; the next generation will adopt them instead of "
		"restarting the pool", name, n);
}
/* }}} */

/* Finds and removes `name`'s "name:pid,pid,..." group from
 * FPM_RELOAD_SELECTIVE_ENV, splitting the pid list into `out_pids` (caller
 * frees). Returns the number of pids, 0 if `name` had no entry. */
static int fpm_reload_selective_env_take(const char *name, pid_t **out_pids) /* {{{ */
{
	const char *existing = getenv(FPM_RELOAD_SELECTIVE_ENV);
	size_t name_len = strlen(name);
	char *copy, *rest, *kept;
	char *found_pids = NULL;
	pid_t *pids = NULL;
	int n = 0;

	*out_pids = NULL;
	if (!existing || !*existing) {
		return 0;
	}

	copy = strdup(existing);
	kept = malloc(strlen(existing) + 1);
	if (!copy || !kept) {
		free(copy);
		free(kept);
		return 0;
	}
	kept[0] = '\0';

	rest = copy;
	while (rest && *rest) {
		char *semi = strchr(rest, ';');
		char *group = rest;

		if (semi) {
			*semi = '\0';
			rest = semi + 1;
		} else {
			rest = NULL;
		}

		if (!found_pids && strncmp(group, name, name_len) == 0 && group[name_len] == ':') {
			found_pids = strdup(group + name_len + 1);
			continue; /* drop this group from `kept` */
		}

		if (*group) {
			if (*kept) {
				strcat(kept, ";");
			}
			strcat(kept, group);
		}
	}

	if (found_pids) {
		if (*kept) {
			setenv(FPM_RELOAD_SELECTIVE_ENV, kept, 1);
		} else {
			unsetenv(FPM_RELOAD_SELECTIVE_ENV);
		}

		/* Count pids first so the array can be sized exactly. */
		for (rest = found_pids; *rest; ) {
			char *comma = strchr(rest, ',');

			n++;
			rest = comma ? comma + 1 : rest + strlen(rest);
		}

		/* Only allocate when there is something to carry: a malformed group
		 * ("name:" with an empty pid list) leaves n == 0, and handing the
		 * caller a non-NULL array alongside a zero count would leak it in
		 * fpm_reload_selective_adopt(), which returns on n == 0. */
		if (n > 0) {
			pids = malloc(sizeof(pid_t) * (size_t) n);
		}
		if (pids) {
			int i = 0;

			for (rest = found_pids; *rest && i < n; i++) {
				char *comma = strchr(rest, ',');

				if (comma) {
					*comma = '\0';
				}
				pids[i] = (pid_t) strtol(rest, NULL, 10);
				rest = comma ? comma + 1 : rest + strlen(rest);
			}
			*out_pids = pids;
		} else {
			n = 0;
		}
		free(found_pids);
	}

	free(copy);
	free(kept);
	return n;
}
/* }}} */

void fpm_reload_selective_adopt(struct fpm_worker_pool_s *wp) /* {{{ */
{
	pid_t *pids = NULL;
	int n = fpm_reload_selective_env_take(wp->config->name, &pids);
	int adopted = 0;
	int i;

	if (n == 0) {
		return;
	}

	for (i = 0; i < n; i++) {
		struct timeval started;

		/* A dead-on-arrival pid (exited in the window between being spared
		 * and this call) must not be adopted: fpm_children_adopt() has no way
		 * to learn it is already gone other than a future SIGCHLD, and
		 * fpm_children_bury() will still reap and correctly account for it
		 * AS LONG AS it was never linked in the first place -- adopting a
		 * corpse would double-count it (linked once here, then unlinked again
		 * on reap) and, worse, would make the ordinary fork loop right after
		 * this function returns think this slot is already covered. Signal 0
		 * is the standard existence probe and disturbs nothing if it is. */
		if (kill(pids[i], 0) != 0) {
			continue;
		}

		fpm_clock_get(&started);
		if (fpm_children_adopt(wp, pids[i], started)) {
			adopted++;
		}
	}

	free(pids);

	if (adopted > 0) {
		zlog(ZLOG_NOTICE, "[pool %s] issue #330: adopted %d child(ren) carried over by "
			"a selective reload; not restarted", wp->config->name, adopted);
	}
}
/* }}} */
