/* fpm-ng: issue #330 -- see fpm_reload_selective.h for the design. */

#include "fpm_config.h"

#include <sys/types.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fpm_reload_selective.h"
#include "fpm_reload_selective_guard.h"
#include "fpm_reload_shm.h"
#include "fpm_worker_pool.h"
#include "fpm_children.h"
#include "fpm_children_extra.h"
#include "fpm_pid_child.h"
#include "fpm_adopted_sweep.h"
#include "fpm_clock.h"
#include "fpm_fault_inject.h"
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

/* Issue #891: each pid is carried as "pid/start", start being its start time
 * (fpm_pid_start_time()), so the next generation can tell the same process
 * from a recycled pid. Under daemonize = yes the next generation is not the
 * parent of these children, so parentage cannot make that distinction. A
 * start of 0 means the time was not recorded (no /proc); see fpm_pid_child.c. */
struct fpm_reload_selective_carried_s {
	pid_t pid;
	unsigned long long start;
};

static int fpm_reload_selective_name_ok(const char *name) /* {{{ */
{
	return !strpbrk(name, ":,;");
}
/* }}} */

/* Sends SIGTERM to every "pid/start" entry of a list and returns how many
 * signals were sent. Used for children that the handoff cannot carry (issue
 * #830): they are detached, so no master would ever adopt or signal them. */
static int fpm_reload_selective_sigterm_list(const char *list) /* {{{ */
{
	const char *p = list;
	int killed = 0;

	while (*p) {
		char *end;
		long pid = strtol(p, &end, 10);
		unsigned long long start = 0;

		if (*end == '/') {
			start = strtoull(end + 1, &end, 10);
		}

		/* pid <= 1 is never a spared worker, and kill(2) reads 0 and -1 as
		 * "a whole process group" and "everything" (issue #567). */
		/* Issue #891: only the process that was spared. A pid that was reaped
		 * earlier may belong to another process by now. */
		if (pid > 1 && fpm_pid_is_same((pid_t) pid, start) && kill((pid_t) pid, SIGTERM) == 0) {
			killed++;
		}

		/* Skip what is left of this entry, so that the loop always advances. */
		p = end;
		while (*p && *p != ',') {
			p++;
		}
		if (*p == ',') {
			p++;
		}
	}

	return killed;
}
/* }}} */

int fpm_reload_selective_spare_pool(struct fpm_worker_pool_s *wp) /* {{{ */
{
	const char *name = wp->config->name;
	const char *existing;
	char *pids, *env_next;
	size_t pids_len = 0, env_cap;
	struct fpm_child_s *child;
	int n = 0;

	if (!fpm_reload_selective_name_ok(name)) {
		zlog(ZLOG_WARNING, "[pool %s] issue #330: pool name contains ':', ',' or ';', "
						   "cannot carry this pool's children across a selective reload -- "
						   "reloading it normally instead",
				name);
		return 0;
	}

	if (wp->running_children == 0) {
		return 1; /* nothing to spare */
	}

	/* Issue #830: both buffers are allocated and sized before the first detach.
	 * After a detach nothing may fail except setenv(): a detached child that
	 * cannot be listed has no master that would adopt or signal it. An entry
	 * takes at most 11 bytes for the pid ("-2147483648"), one '/', 20 bytes for
	 * the start time (unsigned long long) and one ',' more: 33. */
	existing = getenv(FPM_RELOAD_SELECTIVE_ENV);
	env_cap = (existing ? strlen(existing) : 0) + strlen(name) + (size_t) wp->running_children * 33 + 3;
	pids = malloc((size_t) wp->running_children * 33 + 1);
	env_next = malloc(env_cap);
	if (!pids || !env_next) {
		/* Nothing is detached, so these children are still this master's.
		 * Returning 0 sends the pool down the ordinary reload path, which
		 * signals them. */
		free(pids);
		free(env_next);
		return 0;
	}
	pids[0] = '\0';

	/* Build "pid,pid,pid" by repeatedly detaching the oldest remaining child
	 * (fpm_children_detach_oldest() already exists for #329 and does exactly
	 * the unlink-without-signalling this needs; calling it in a loop until
	 * wp->children is empty is the whole of "detach every child" -- no new
	 * detach-all primitive needed). The list holds running_children entries
	 * (fpm_child_unlink() decrements both), so pids is large enough. */
	while ((child = fpm_children_detach_oldest(wp)) != NULL) {
		char one[48];
		int one_len;

		one_len = snprintf(one, sizeof(one), "%s%d/%llu", n ? "," : "", (int) child->pid,
				fpm_pid_start_time(child->pid));
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
		free(env_next);
		return 1;
	}

	if (existing && *existing) {
		snprintf(env_next, env_cap, "%s;%s:%s", existing, name, pids);
	} else {
		snprintf(env_next, env_cap, "%s:%s", name, pids);
	}

	if (setenv(FPM_RELOAD_SELECTIVE_ENV, env_next, 1) != 0) {
		/* The children are detached and the next generation cannot adopt
		 * them either. Signal them, as for a child that could not be listed
		 * (issue #830). No shm record is written either, so the next
		 * generation starts this pool with a fresh scoreboard. Returning 0
		 * makes the caller's ordinary path see no child of this pool. */
		fpm_reload_selective_sigterm_list(pids);
		free(pids);
		free(env_next);
		return 0;
	}
	free(env_next);

	/* Issue #537: the spared workers keep writing to this generation's
	 * scoreboard and metrics region; hand both to the next generation. Only
	 * after the handoff succeeded: a scoreboard record without its workers
	 * would fill the next generation's scoreboard with dead slots (issue #830). */
	fpm_reload_shm_spare_pool(wp);
	free(pids);

	zlog(ZLOG_NOTICE, "[pool %s] issue #330: config unchanged -- sparing all %d running "
					  "child(ren) from this reload; the next generation will adopt them instead of "
					  "restarting the pool",
			name, n);
	return 1;
}
/* }}} */

/* Finds and removes `name`'s "name:pid/start,pid/start,..." group from
 * FPM_RELOAD_SELECTIVE_ENV, splitting the list into `out_pids` (caller
 * frees). Returns the number of entries, 0 if `name` had no entry. */
static int fpm_reload_selective_env_take(const char *name, struct fpm_reload_selective_carried_s **out_pids) /* {{{ */
{
	const char *existing = getenv(FPM_RELOAD_SELECTIVE_ENV);
	size_t name_len = strlen(name);
	char *copy, *rest, *kept;
	char *found_pids = NULL;
	struct fpm_reload_selective_carried_s *pids = NULL;
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
		for (rest = found_pids; *rest;) {
			char *comma = strchr(rest, ',');

			n++;
			rest = comma ? comma + 1 : rest + strlen(rest);
		}

		/* Only allocate when there is something to carry: a malformed group
		 * ("name:" with an empty pid list) leaves n == 0, and handing the
		 * caller a non-NULL array alongside a zero count would leak it in
		 * fpm_reload_selective_adopt(), which returns on n == 0. */
		if (n > 0) {
			pids = malloc(sizeof(*pids) * (size_t) n);
		}
		if (pids) {
			int i = 0;

			for (rest = found_pids; *rest && i < n; i++) {
				char *comma = strchr(rest, ',');
				char *slash;

				if (comma) {
					*comma = '\0';
				}
				pids[i].pid = (pid_t) strtol(rest, &slash, 10);
				/* No '/' means an entry from a binary that predates #891: start 0. */
				pids[i].start = (*slash == '/') ? strtoull(slash + 1, NULL, 10) : 0;
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
	struct fpm_reload_selective_carried_s *pids = NULL;
	int n = fpm_reload_selective_env_take(wp->config->name, &pids);
	int adopted = 0;
	int i;

	if (n == 0) {
		return;
	}

	/* Issue #829: a test point that holds this master here. The entries have
	 * left the environment and no child is adopted yet, so this is the window
	 * the guard covers: a master killed now leaves the workers to the guard.
	 * The hold is bounded (30 seconds), so a test cannot leave a master stuck. */
	if (FPM_FAULT_POINT("selective_adopt_hold")) {
		int s;

		for (s = 0; s < 30; s++) {
			sleep(1);
		}
	}

	for (i = 0; i < n; i++) {
		struct timeval started;
		struct fpm_child_s *child;

		/* A child that exited in the window between being spared and this
		 * call must not be adopted: fpm_children_adopt() would link a corpse,
		 * and the ordinary fork loop right after this function returns would
		 * count the slot as covered. The check does not reap it. Under
		 * fpm_children_bury(), which calls waitpid(-1), a dead child of this
		 * master is still reaped there even though it was never adopted.
		 * Issue #891: kill(pid, 0) is not enough. A pid reaped in the window
		 * may now belong to an unrelated process, and adopting it would later
		 * signal that process. fpm_pid_is_same() checks the start time. */
		if (!fpm_pid_is_same(pids[i].pid, pids[i].start)) {
			zlog(ZLOG_NOTICE, "[pool %s] issue #891: selective reload carried pid %d is not the "
							  "process it was when spared (it exited, or its pid was reused); "
							  "not adopting it",
					wp->config->name, (int) pids[i].pid);
			/* Its scoreboard slot is still marked used in the inherited
			 * scoreboard (issue #537) and nothing else would free it. */
			fpm_reload_shm_drop_slot(wp, pids[i].pid);
			continue;
		}

		fpm_clock_get(&started);
		/* Issue #537: the worker writes to the scoreboard slot its previous
		 * master gave it; make adoption take that one. */
		fpm_reload_shm_prepare_adopt(wp, pids[i].pid);
		child = fpm_children_adopt(wp, pids[i].pid, started);
		if (child) {
			adopted++;
			/* Issue #792: under daemonize = yes this master never gets the
			 * SIGCHLD of the carried child. Poll for its exit. */
			fpm_adopted_sweep_register(child, pids[i].start ? pids[i].start : fpm_pid_start_time(pids[i].pid));
		}
	}

	free(pids);

	if (adopted > 0) {
		zlog(ZLOG_NOTICE, "[pool %s] issue #330: adopted %d child(ren) carried over by "
						  "a selective reload; not restarted",
				wp->config->name, adopted);
	}
}
/* }}} */

int fpm_reload_selective_has_carried(void) /* {{{ */
{
	const char *existing = getenv(FPM_RELOAD_SELECTIVE_ENV);

	return existing && *existing;
}
/* }}} */

int fpm_reload_selective_sigterm_carried(void) /* {{{ */
{
	const char *existing = getenv(FPM_RELOAD_SELECTIVE_ENV);
	char *copy, *rest;
	int killed = 0;

	if (!existing || !*existing) {
		return 0;
	}

	copy = strdup(existing);
	/* Drop the variable first: whatever happens below, no later code path in
	 * this process may find these pids again and treat them as its own. */
	unsetenv(FPM_RELOAD_SELECTIVE_ENV);
	if (!copy) {
		return 0;
	}

	for (rest = copy; rest && *rest;) {
		char *semi = strchr(rest, ';');
		char *group = rest;
		char *colon;

		if (semi) {
			*semi = '\0';
			rest = semi + 1;
		} else {
			rest = NULL;
		}

		colon = strchr(group, ':');
		if (!colon) {
			continue;
		}

		killed += fpm_reload_selective_sigterm_list(colon + 1);
	}

	free(copy);
	return killed;
}
/* }}} */

void fpm_reload_selective_discard_unadopted(void) /* {{{ */
{
	int killed = fpm_reload_selective_sigterm_carried();

	/* Also called from fpm_run() after the initial fork loop, where this
	 * master keeps running. The message says only what is true in both
	 * places: this master did not adopt them (issue #830). */
	if (killed > 0) {
		zlog(ZLOG_WARNING, "issue #690: this master did not adopt %d child(ren) "
						   "that a selective reload carried over; sent them SIGTERM so they do not "
						   "keep running with their listening sockets",
				killed);
	}

	/* Issue #829: the guard is released last, after the SIGTERMs above. A
	 * master that stops inside this function still leaves the guard armed, and
	 * the guard then signals the same pids again, which the start-time check
	 * makes harmless. Also reached when nothing was carried: the release must
	 * not depend on the list. */
	fpm_reload_selective_guard_release();
}
/* }}} */
