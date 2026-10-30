/* fpm-ng: crash backoff for pm children that fail fast (issue #727). The rules
 * are in fpm_crash_backoff.h. */

#include "fpm_config.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "fpm.h"
#include "fpm_children.h"
#include "fpm_clock.h"
#include "fpm_conf.h"
#include "fpm_events.h"
#include "fpm_process_ctl.h"
#include "fpm_scoreboard.h"
#include "fpm_shm.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_type.h"
#include "fpm_operator_http.h"
#include "fpm_crash_backoff.h"
#include "zlog.h"

/* The longest delay between two respawns of a failing pool, in ms. */
#define FPM_CRASH_BACKOFF_CAP_MS 60000UL

/* Read by the operator endpoint, so it lives in shared memory. */
struct fpm_crash_backoff_shared_s {
	unsigned consecutive_failures;
	unsigned gave_up;
	unsigned long respawn_delay_ms;
};

/* Master-only state for one pool. Allocated before the first fork, so that the
 * operator endpoint's copy of this list reaches the same shared block. */
struct fpm_crash_backoff_entry_s {
	struct fpm_worker_pool_s *wp;
	struct fpm_crash_backoff_shared_s *shared;

	/* The respawn may not happen before this monotonic time. */
	struct timeval next_spawn;

	/* One-shot timer that calls fpm_children_make() at gate_due. The event is
	 * never fpm_event_del()'d: the timer walk in fpm_event_loop() keeps a pointer
	 * to the next node, and a delete from a callback can free that node. A new
	 * deadline is set with fpm_event_add() on the queued node instead. */
	struct fpm_event_s gate_ev;
	struct timeval gate_due;
	int gate_armed; /* 1 while gate_ev is queued */

	/* One-shot timer that checks, after a full window, the child spawned most
	 * recently during a streak. Re-armed in place, never deleted (see gate_ev). */
	struct fpm_event_s survive_ev;
	int survive_armed;
	pid_t watch_pid;
	struct timeval watch_started;

	struct fpm_crash_backoff_entry_s *next;
};

static struct fpm_crash_backoff_entry_s *fpm_crash_backoff_entries;

static struct fpm_crash_backoff_entry_s *fpm_crash_backoff_find(const struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_crash_backoff_entry_s *e;

	for (e = fpm_crash_backoff_entries; e; e = e->next) {
		if (e->wp == wp) {
			return e;
		}
	}
	return NULL;
}
/* }}} */

/* A value in [0, max]. Hashed from the clock and the pid, not taken from rand():
 * libc's state survives fork(), so every child would continue the same sequence.
 * The same reasoning is in fpm_pool_supervisor.c. The mixing step is splitmix64;
 * no cryptographic property is needed for a delay. */
static unsigned long fpm_crash_backoff_jitter(unsigned long max) /* {{{ */
{
	struct timeval now;
	uint64_t x;

	if (max == 0) {
		return 0;
	}

	fpm_clock_get(&now);
	x = ((uint64_t) now.tv_sec << 32) ^ (uint64_t) now.tv_usec ^ ((uint64_t) getpid() << 48);
	x += 0x9e3779b97f4a7c15ULL;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	x ^= x >> 31;

	return (unsigned long) (x % ((uint64_t) max + 1));
}
/* }}} */

/* The respawn delay after the fast failure that made the streak `failures`
 * long. The first failure respawns at once. Later ones wait a window that
 * doubles from 1 s up to the cap. After the pool gave up, the window is the cap,
 * so the retries run at the capped rate. The delay is drawn from the upper half
 * of the window, so it never falls to zero and two pools do not retry in step. */
static unsigned long fpm_crash_backoff_delay_ms(unsigned failures, int gave_up) /* {{{ */
{
	unsigned long window;
	unsigned shift;

	if (gave_up) {
		window = FPM_CRASH_BACKOFF_CAP_MS;
	} else if (failures < 2) {
		return 0;
	} else {
		shift = failures - 2;
		if (shift > 6) {
			shift = 6;
		}
		window = 1000UL << shift;
		if (window > FPM_CRASH_BACKOFF_CAP_MS) {
			window = FPM_CRASH_BACKOFF_CAP_MS;
		}
	}

	return window / 2 + fpm_crash_backoff_jitter(window - window / 2);
}
/* }}} */

static void fpm_crash_backoff_clear(struct fpm_crash_backoff_entry_s *e) /* {{{ */
{
	e->shared->consecutive_failures = 0;
	e->shared->gave_up = 0;
	e->shared->respawn_delay_ms = 0;
	timerclear(&e->next_spawn);
}
/* }}} */

/* One-shot timer callback. The timer is already removed when this runs, so
 * clear the flag, then let fpm_children_make() ask fpm_crash_backoff_may_spawn()
 * again. If the time has not come, that call arms the timer once more. */
static void fpm_crash_backoff_gate_fire(struct fpm_event_s *ev, short which, void *arg) /* {{{ */
{
	struct fpm_crash_backoff_entry_s *e = arg;

	(void) ev;
	(void) which;
	e->gate_armed = 0;
	fpm_children_make(e->wp, 1 /* in event loop */, 1, 0);
}
/* }}} */

/* One-shot timer callback. The child that started the watch is still alive after
 * a full window, so the pool is healthy again. A child that was started later
 * has a different start time, and the check below sees that as a different
 * child. */
static void fpm_crash_backoff_survive_fire(struct fpm_event_s *ev, short which, void *arg) /* {{{ */
{
	struct fpm_crash_backoff_entry_s *e = arg;
	struct fpm_child_s *child;

	(void) ev;
	(void) which;
	e->survive_armed = 0;

	if (e->shared->consecutive_failures == 0) {
		return;
	}

	child = fpm_child_find(e->watch_pid);
	if (!child || child->wp != e->wp || !timercmp(&child->started, &e->watch_started, ==)) {
		return;
	}

	zlog(ZLOG_NOTICE, "[pool %s] crash streak of %u ended: child %d has lived a whole window",
			e->wp->config->name, e->shared->consecutive_failures, (int) e->watch_pid);
	/* The gate timer may stay queued. Its callback then finds no wait and only
	 * calls fpm_children_make(), which is harmless. */
	fpm_crash_backoff_clear(e);
	fpm_children_make(e->wp, 1 /* in event loop */, 1, 0);
}
/* }}} */

int fpm_crash_backoff_init(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_crash_backoff_entry_s *e;

	if (!fpm_pool_type_of(wp)->respawn_backoff) {
		return 0;
	}

	e = calloc(1, sizeof(*e));
	if (!e) {
		zlog(ZLOG_ERROR, "[pool %s] unable to malloc the crash backoff state", wp->config->name);
		return -1;
	}

	/* fpm_shm_alloc() is zero-filled, so the streak starts clear. */
	e->shared = fpm_shm_alloc(sizeof(*e->shared));
	if (!e->shared) {
		zlog(ZLOG_ERROR, "[pool %s] unable to allocate the crash backoff shared state", wp->config->name);
		free(e);
		return -1;
	}

	e->wp = wp;
	fpm_event_set_timer(&e->gate_ev, 0, fpm_crash_backoff_gate_fire, e);
	fpm_event_set_timer(&e->survive_ev, 0, fpm_crash_backoff_survive_fire, e);
	e->next = fpm_crash_backoff_entries;
	fpm_crash_backoff_entries = e;
	return 0;
}
/* }}} */

void fpm_crash_backoff_child_exited(struct fpm_child_s *child) /* {{{ */
{
	struct fpm_crash_backoff_entry_s *e = fpm_crash_backoff_find(child->wp);
	struct fpm_scoreboard_proc_s *proc;
	struct timeval now, life, delta;
	unsigned requests = 0;
	unsigned long delay_ms;
	unsigned failures;
	int max;

	if (!e || !fpm_pctl_can_spawn_children()) {
		/* Not a pool with backoff, or the master is stopping or reloading: an
		 * exit then says nothing about the pool. */
		return;
	}

	proc = fpm_scoreboard_proc_get_from_child(child);
	if (proc) {
		requests = proc->requests;
	}

	fpm_clock_get(&now);
	timersub(&now, &child->started, &life);

	if (requests > 0 || life.tv_sec >= FPM_CRASH_BACKOFF_WINDOW_S) {
		if (e->shared->consecutive_failures > 0) {
			zlog(ZLOG_NOTICE, "[pool %s] crash streak of %u ended: child %d served %u requests after %ld.%06d seconds",
					child->wp->config->name, e->shared->consecutive_failures, (int) child->pid,
					requests, (long) life.tv_sec, (int) life.tv_usec);
		}
		fpm_crash_backoff_clear(e);
		return;
	}

	e->shared->consecutive_failures++;
	failures = e->shared->consecutive_failures;
	max = child->wp->config->pm_max_consecutive_failures;

	if (max > 0 && failures >= (unsigned) max && !e->shared->gave_up) {
		e->shared->gave_up = 1;
		zlog(ZLOG_ALERT, "[pool %s] gave up after %u children in a row exited within %d seconds without serving a request (pm.max_consecutive_failures = %d); retries continue at most every %lu seconds",
				child->wp->config->name, failures, FPM_CRASH_BACKOFF_WINDOW_S, max, FPM_CRASH_BACKOFF_CAP_MS / 1000UL);
	}

	delay_ms = fpm_crash_backoff_delay_ms(failures, (int) e->shared->gave_up);
	e->shared->respawn_delay_ms = delay_ms;

	delta.tv_sec = (time_t) (delay_ms / 1000UL);
	delta.tv_usec = (suseconds_t) ((delay_ms % 1000UL) * 1000UL);
	timeradd(&now, &delta, &e->next_spawn);

	zlog(ZLOG_WARNING, "[pool %s] child %d exited after %ld.%06d seconds without serving a request (%u in a row); next child in %lu ms",
			child->wp->config->name, (int) child->pid, (long) life.tv_sec, (int) life.tv_usec,
			failures, delay_ms);
}
/* }}} */

int fpm_crash_backoff_may_spawn(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_crash_backoff_entry_s *e = fpm_crash_backoff_find(wp);
	struct timeval now, left;
	unsigned long ms;

	if (!e || !timerisset(&e->next_spawn)) {
		return 1;
	}

	fpm_clock_get(&now);
	if (!timercmp(&now, &e->next_spawn, <)) {
		timerclear(&e->next_spawn);
		return 1;
	}

	/* Round up, so the timer never fires before the time has come. */
	timersub(&e->next_spawn, &now, &left);
	ms = (unsigned long) left.tv_sec * 1000UL + ((unsigned long) left.tv_usec + 999UL) / 1000UL;

	/* Arm the gate, or move it earlier when a streak ended and a shorter delay
	 * is now due. fpm_event_add() on a queued timer only sets its new deadline,
	 * so this may run from inside a timer callback. */
	if (!e->gate_armed || timercmp(&e->next_spawn, &e->gate_due, <)) {
		fpm_event_add(&e->gate_ev, ms);
		e->gate_due = e->next_spawn;
		e->gate_armed = 1;
	}
	return 0;
}
/* }}} */

void fpm_crash_backoff_child_spawned(struct fpm_child_s *child) /* {{{ */
{
	struct fpm_crash_backoff_entry_s *e = fpm_crash_backoff_find(child->wp);

	if (!e) {
		return;
	}

	/* The child that the wait was for is forked now, so the wait is over. */
	e->shared->respawn_delay_ms = 0;

	if (e->shared->consecutive_failures == 0) {
		return;
	}

	e->watch_pid = child->pid;
	e->watch_started = child->started;

	/* Moves the deadline of a queued survive timer to a full window from now. */
	fpm_event_add(&e->survive_ev, FPM_CRASH_BACKOFF_WINDOW_S * 1000UL);
	e->survive_armed = 1;
}
/* }}} */

void fpm_crash_backoff_read(struct fpm_worker_pool_s *wp, struct fpm_crash_backoff_snapshot_s *out) /* {{{ */
{
	const struct fpm_crash_backoff_entry_s *e = fpm_crash_backoff_find(wp);

	memset(out, 0, sizeof(*out));
	if (!e) {
		return;
	}

	/* The master may write while this copies. Each field is one word, so a
	 * read can show a value from just before a change; a page is a snapshot. */
	out->has_state = 1;
	out->consecutive_failures = e->shared->consecutive_failures;
	out->gave_up = (int) e->shared->gave_up;
	out->respawn_delay_ms = e->shared->respawn_delay_ms;
}
/* }}} */

void fpm_crash_backoff_render_prometheus(struct fpm_operator_buf_s *b, const char *pool,
		const struct fpm_crash_backoff_snapshot_s *snap) /* {{{ */
{
	if (!snap->has_state) {
		return;
	}
	fpm_operator_buf_appendf(b,
			"fpmng_pool_consecutive_crashes{pool=\"%s\"} %u\n"
			"fpmng_pool_crash_gave_up{pool=\"%s\"} %d\n"
			"fpmng_pool_respawn_delay_ms{pool=\"%s\"} %lu\n",
			pool, snap->consecutive_failures,
			pool, snap->gave_up,
			pool, snap->respawn_delay_ms);
}
/* }}} */

void fpm_crash_backoff_render_json(struct fpm_operator_buf_s *b, const struct fpm_crash_backoff_snapshot_s *snap) /* {{{ */
{
	if (!snap->has_state) {
		return;
	}
	fpm_operator_buf_appendf(b, ",\"consecutive_crashes\":%u,\"crash_gave_up\":%s,\"respawn_delay_ms\":%lu",
			snap->consecutive_failures, snap->gave_up ? "true" : "false", snap->respawn_delay_ms);
}
/* }}} */
