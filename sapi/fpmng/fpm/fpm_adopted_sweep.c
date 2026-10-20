/* fpm-ng: issue #792 -- see fpm_adopted_sweep.h. */

#include "fpm_config.h"

#include <stdlib.h>
#include <unistd.h>

#include "fpm_adopted_sweep.h"
#include "fpm_children.h"
#include "fpm_children_extra.h"
#include "fpm_events.h"
#include "fpm_pid_child.h"
#include "fpm.h"

#define FPM_ADOPTED_SWEEP_INTERVAL_MS 1000

/* Under daemonize = yes the exec'd image forks again
 * (third_party/php-src/sapi/fpm/fpm/fpm_unix.c:618), so the new master is not
 * the parent of a child that a selective reload carried over (#330). Its exit
 * raises no SIGCHLD in this master and waitpid(-1) never returns its pid, so
 * fpm_children_bury() never unlinks it: running_children never reaches 0 and a
 * SIGTERM never completes (issue #792), and while the master runs the slot
 * counts as covered, so a pm = static pool is not refilled. This sweep asks
 * the kernel instead, by start time (fpm_pid_is_same(), issue #891), and runs
 * the exit path for a child whose process is gone.
 *
 * The entry also holds the child pointer and `started`. fpm_children_bury()
 * reaps a child that IS ours (no daemonize) first, and the kernel can give its
 * pid to a child forked later: fpm_child_find() then returns another struct,
 * which is never retired here. */
struct sweep_entry_s {
	struct sweep_entry_s *next;
	struct fpm_child_s *child;
	pid_t pid;
	unsigned long long start;
	struct timeval started;
};

static struct sweep_entry_s *entries;
static struct fpm_event_s sweep_event;
static int sweep_armed;

static void fpm_adopted_sweep_tick(struct fpm_event_s *ev, short which, void *arg);

static void fpm_adopted_sweep_disarm(void)
{
	if (sweep_armed) {
		fpm_event_del(&sweep_event);
		sweep_armed = 0;
	}
}

void fpm_adopted_sweep_register(struct fpm_child_s *child, unsigned long long start) /* {{{ */
{
	struct sweep_entry_s *e;

	char state;
	pid_t ppid;

	if (!child) {
		return;
	}

	/* A child of this master (no daemonize) gets its SIGCHLD, and
	 * fpm_children_bury() is its only owner: a sweep that retired its zombie
	 * first would make bury log "unknown child" and skip the crash backoff. */
	if (fpm_pid_proc_state(child->pid, &state, &ppid) && ppid == getpid()) {
		return;
	}

	e = malloc(sizeof(*e));
	if (!e) {
		return;
	}

	e->child = child;
	e->pid = child->pid;
	e->start = start;
	e->started = child->started;
	e->next = entries;
	entries = e;

	if (!sweep_armed) {
		fpm_event_set_timer(&sweep_event, FPM_EV_PERSIST, &fpm_adopted_sweep_tick, NULL);
		fpm_event_add(&sweep_event, FPM_ADOPTED_SWEEP_INTERVAL_MS);
		sweep_armed = 1;
	}
}
/* }}} */

/* A process that exited stays a zombie until its parent reaps it, and a
 * zombie still has its /proc entry, its start time and answers kill(pid, 0).
 * After the daemonize fork the parent is init, or whatever is PID 1 in a
 * container, and a PID 1 that does not reap (the CI job's `tail -f /dev/null`)
 * leaves the corpse for good: fpm_pid_is_same() alone would call it alive and
 * a SIGTERM would never complete. Z and X are therefore gone. */
static int fpm_adopted_sweep_alive(pid_t pid, unsigned long long start)
{
	char state;
	pid_t ppid;

	if (fpm_pid_proc_state(pid, &state, &ppid) && (state == 'Z' || state == 'X')) {
		return 0;
	}

	return fpm_pid_is_same(pid, start);
}

/* Detaches and returns the first entry whose process is gone, dropping the
 * entries whose child fpm_children_bury() already retired. */
static struct fpm_child_s *fpm_adopted_sweep_take_gone(void)
{
	struct sweep_entry_s **link = &entries;

	while (*link) {
		struct sweep_entry_s *e = *link;
		struct fpm_child_s *current = fpm_child_find(e->pid);
		int tracked = current == e->child && timercmp(&current->started, &e->started, ==);

		if (tracked && fpm_adopted_sweep_alive(e->pid, e->start)) {
			link = &e->next;
			continue;
		}

		*link = e->next;
		free(e);
		if (tracked) {
			return current;
		}
	}

	return NULL;
}

static void fpm_adopted_sweep_tick(struct fpm_event_s *ev, short which, void *arg) /* {{{ */
{
	struct fpm_child_s *gone;

	if (fpm_globals.parent_pid != getpid()) {
		return; /* a worker that inherited the event: the master's job */
	}

	while ((gone = fpm_adopted_sweep_take_gone()) != NULL) {
		/* May exit the master (last child of a stop) or fork a replacement. */
		fpm_children_adopted_exited(gone);

		if (fpm_globals.is_child) {
			return;
		}
	}

	if (!entries) {
		fpm_adopted_sweep_disarm();
	}
}
/* }}} */
