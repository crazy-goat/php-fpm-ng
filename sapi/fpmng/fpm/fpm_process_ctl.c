	/* (c) 2007,2008 Andrei Nigmatulin */

#include "fpm_config.h"

#include <sys/types.h>
#include <sys/time.h>		/* timeradd, timercmp */
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>

#include "fpm.h"
#include "fpm_clock.h"
#include "fpm_children.h"
#include "fpm_signals.h"
#include "fpm_events.h"
#include "fpm_process_ctl.h"
#include "fpm_pctl_retire.h"
#include "fpm_cleanup.h"
#include "fpm_request.h"
#include "fpm_pool_type.h"
#include "fpm_worker_pool.h"
#include "fpm_scoreboard.h"
#include "fpm_sockets.h"
#include "fpm_stdio.h"
#include "zlog.h"


static int fpm_state = FPM_PCTL_STATE_NORMAL;
static int fpm_signal_sent = 0;


static const char *fpm_state_names[] = {
	[FPM_PCTL_STATE_NORMAL] = "normal",
	[FPM_PCTL_STATE_RELOADING] = "reloading",
	[FPM_PCTL_STATE_TERMINATING] = "terminating",
	[FPM_PCTL_STATE_FINISHING] = "finishing"
};

static int saved_argc;
static char **saved_argv;

static void fpm_pctl_cleanup(int which, void *arg) /* {{{ */
{
	int i;
	if (which != FPM_CLEANUP_PARENT_EXEC) {
		for (i = 0; i < saved_argc; i++) {
			free(saved_argv[i]);
		}
		free(saved_argv);
	}
}
/* }}} */

static struct fpm_event_s pctl_event;

static void fpm_pctl_action(struct fpm_event_s *ev, short which, void *arg) /* {{{ */
{
	fpm_pctl(FPM_PCTL_STATE_UNSPECIFIED, FPM_PCTL_ACTION_TIMEOUT);
}
/* }}} */

static int fpm_pctl_timeout_set(int sec) /* {{{ */
{
	fpm_event_set_timer(&pctl_event, 0, &fpm_pctl_action, NULL);
	fpm_event_add(&pctl_event, sec * 1000);
	return 0;
}
/* }}} */

static void fpm_pctl_exit(void)
{
	zlog(ZLOG_NOTICE, "exiting, bye-bye!");

	fpm_conf_unlink_pid();
	fpm_cleanups_run(FPM_CLEANUP_PARENT_EXIT_MAIN);
	exit(FPM_EXIT_OK);
}

#define optional_arg(c) ((saved_argc) > (c) ? ", \"" : ""), ((saved_argc) > (c) ? (saved_argv)[(c)] : ""), ((saved_argc) > (c) ? "\"" : "")

static void fpm_pctl_exec(void)
{
	zlog(ZLOG_DEBUG, "Blocking some signals before reexec");
	if (0 > fpm_signals_block()) {
		zlog(ZLOG_WARNING, "concurrent reloads may be unstable");
	}

	zlog(ZLOG_NOTICE, "reloading: execvp(\"%s\", {\"%s\""
			"%s%s%s" "%s%s%s" "%s%s%s" "%s%s%s" "%s%s%s"
			"%s%s%s" "%s%s%s" "%s%s%s" "%s%s%s" "%s%s%s"
		"})",
		saved_argv[0], saved_argv[0],
		optional_arg(1),
		optional_arg(2),
		optional_arg(3),
		optional_arg(4),
		optional_arg(5),
		optional_arg(6),
		optional_arg(7),
		optional_arg(8),
		optional_arg(9),
		optional_arg(10)
	);

	fpm_cleanups_run(FPM_CLEANUP_PARENT_EXEC);

	fpm_stdio_restore_original_stderr(1);

	execvp(saved_argv[0], saved_argv);
	zlog(ZLOG_SYSERROR, "failed to reload: execvp() failed");
	exit(FPM_EXIT_SOFTWARE);
}

static void fpm_pctl_action_last(void)
{
	switch (fpm_state) {
		case FPM_PCTL_STATE_RELOADING:
			fpm_pctl_exec();
			break;

		case FPM_PCTL_STATE_FINISHING:
		case FPM_PCTL_STATE_TERMINATING:
			fpm_pctl_exit();
			break;
	}
}

int fpm_pctl_kill(pid_t pid, int how) /* {{{ */
{
	int s = 0;

	switch (how) {
		case FPM_PCTL_TERM :
			s = SIGTERM;
			break;
		case FPM_PCTL_STOP :
			s = SIGSTOP;
			break;
		case FPM_PCTL_CONT :
			s = SIGCONT;
			break;
		case FPM_PCTL_QUIT :
			s = SIGQUIT;
			break;
		case FPM_PCTL_KILL:
			s = SIGKILL;
			break;
		default :
			break;
	}
	return kill(pid, s);
}
/* }}} */

void fpm_pctl_kill_all(int signo) /* {{{ */
{
	struct fpm_worker_pool_s *wp;
	int alive_children = 0;

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		struct fpm_child_s *child;

		for (child = wp->children; child; child = child->next) {
			int child_signo = signo;
			int res;

			/* A reload is a special kind of graceful shutdown. Request
			 * workers receive SIGQUIT so their current request can drain.
			 * Every other pool type (serves_requests == 0) gets SIGTERM
			 * instead, for one of two different reasons:
			 *   - supervisor, cron: each has its own SIGTERM handler that
			 *     finishes the current iteration and prevents another one;
			 *   - the operator endpoint: no SIGTERM handler and no work to
			 *     finish, so SIGTERM's default disposition just exits it
			 *     immediately. Sending SIGTERM directly here also removes a
			 *     delay that would otherwise be accepted: waiting for the
			 *     master to escalate SIGQUIT to SIGTERM.
			 * Keep the ordinary SIGQUIT fan-out unchanged for log rotation
			 * and an explicit graceful stop. */
			if (fpm_state == FPM_PCTL_STATE_RELOADING && signo == SIGQUIT) {
				const struct fpm_pool_type_s *type = fpm_pool_type_of(child->wp);

				if (type && !type->serves_requests && type->child_main) {
					child_signo = SIGTERM;
				}
			}

			res = kill(child->pid, child_signo);

			zlog(ZLOG_DEBUG, "[pool %s] sending signal %d %s to child %d",
				child->wp->config->name, child_signo,
				fpm_signal_names[child_signo] ? fpm_signal_names[child_signo] : "", (int) child->pid);

			if (res == 0) {
				++alive_children;
			}
		}
	}

	if (alive_children) {
		zlog(ZLOG_DEBUG, "%d child(ren) still alive", alive_children);
	}
}
/* }}} */

static void fpm_pctl_action_next(void)
{
	int sig, timeout;

	if (!fpm_globals.running_children) {
		fpm_pctl_action_last();
	}

	if (fpm_signal_sent == 0) {
		if (fpm_state == FPM_PCTL_STATE_TERMINATING) {
			sig = SIGTERM;
		} else {
			sig = SIGQUIT;
		}
		timeout = fpm_global_config.process_control_timeout;
	} else {
		if (fpm_signal_sent == SIGQUIT) {
			sig = SIGTERM;
		} else {
			sig = SIGKILL;
		}
		timeout = 1;
	}

	fpm_pctl_kill_all(sig);
	fpm_signal_sent = sig;
	fpm_pctl_timeout_set(timeout);
}

void fpm_pctl(int new_state, int action) /* {{{ */
{
	switch (action) {
		case FPM_PCTL_ACTION_SET :
			if (fpm_state == new_state) { /* already in progress - just ignore duplicate signal */
				return;
			}

			switch (fpm_state) { /* check which states can be overridden */
				case FPM_PCTL_STATE_NORMAL :
					/* 'normal' can be overridden by any other state */
					break;
				case FPM_PCTL_STATE_RELOADING :
					/* 'reloading' can be overridden by 'finishing' */
					if (new_state == FPM_PCTL_STATE_FINISHING) break;
					ZEND_FALLTHROUGH;
				case FPM_PCTL_STATE_FINISHING :
					/* 'reloading' and 'finishing' can be overridden by 'terminating' */
					if (new_state == FPM_PCTL_STATE_TERMINATING) break;
					ZEND_FALLTHROUGH;
				case FPM_PCTL_STATE_TERMINATING :
					/* nothing can override 'terminating' state */
					zlog(ZLOG_DEBUG, "not switching to '%s' state, because already in '%s' state",
						fpm_state_names[new_state], fpm_state_names[fpm_state]);
					return;
				/* TODO Add EMPTY_SWITCH_DEFAULT_CASE? */
			}

			fpm_signal_sent = 0;
			fpm_state = new_state;

			zlog(ZLOG_DEBUG, "switching to '%s' state", fpm_state_names[fpm_state]);
			ZEND_FALLTHROUGH;

		case FPM_PCTL_ACTION_TIMEOUT :
			fpm_pctl_action_next();
			break;
		case FPM_PCTL_ACTION_LAST_CHILD_EXITED :
			fpm_pctl_action_last();
			break;

	}
}
/* }}} */

int fpm_pctl_can_spawn_children(void)
{
	return fpm_state == FPM_PCTL_STATE_NORMAL;
}

int fpm_pctl_child_exited(void)
{
	if (fpm_state == FPM_PCTL_STATE_NORMAL) {
		return 0;
	}

	if (!fpm_globals.running_children) {
		fpm_pctl(FPM_PCTL_STATE_UNSPECIFIED, FPM_PCTL_ACTION_LAST_CHILD_EXITED);
	}
	return 0;
}

int fpm_pctl_init_main(void)
{
	int i;

	saved_argc = fpm_globals.argc;
	saved_argv = malloc(sizeof(char *) * (saved_argc + 1));

	if (!saved_argv) {
		return -1;
	}

	for (i = 0; i < saved_argc; i++) {
		saved_argv[i] = strdup(fpm_globals.argv[i]);

		if (!saved_argv[i]) {
			return -1;
		}
	}

	saved_argv[i] = 0;

	if (0 > fpm_cleanup_add(FPM_CLEANUP_ALL, fpm_pctl_cleanup, 0)) {
		return -1;
	}
	return 0;
}

static void fpm_pctl_check_request_timeout(struct timeval *now) /* {{{ */
{
	struct fpm_worker_pool_s *wp;

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		int track_finished = wp->config->request_terminate_timeout_track_finished;
		int terminate_timeout = wp->config->request_terminate_timeout;
		int slowlog_timeout = wp->config->request_slowlog_timeout;
		struct fpm_child_s *child;

		if (terminate_timeout || slowlog_timeout) {
			for (child = wp->children; child; child = child->next) {
				fpm_request_check_timed_out(child, now, terminate_timeout, slowlog_timeout, track_finished);
			}
		}
	}
}
/* }}} */

/* Issue #166 (THROWAWAY). The bound the master grants a retiring child, kept
 * here rather than on struct fpm_child_s, which is upstream's. Capacity is
 * pm.max_children's own ceiling on a sane configuration; a table that fills is
 * reported once and then falls back to the unmodified escalation, because
 * losing the bound is better than losing the scale-down. */
#define FPM_PCTL_RETIRE_MAX 4096

/* How much longer than the child's own drain deadline the master waits before
 * it starts escalating. The child's deadline is http.read_timeout measured
 * from the tick on which it noticed the signal (fpm_http_direct.c,
 * fpm_direct_retire_enter); the master's is measured from the tick on which it
 * sent the signal. The slack covers the difference plus one 10 ms child tick,
 * so that an expiry recorded here means the child really did not leave rather
 * than that the two clocks were started a few milliseconds apart. */
#define FPM_PCTL_RETIRE_SLACK_MS 250

/* And how long after SIGQUIT the master waits before SIGKILL, once the bound
 * has expired and escalation has begun. One maintenance pass, which is what
 * the unmodified code gives (the pass rate is the escalation delay there). */
#define FPM_PCTL_RETIRE_QUIT_MS 1000

/* The bound for a pool that set http.read_timeout = 0. Not infinity, and not
 * one pass either: it is the directive's own documented default, which is the
 * only number in the configuration that says how long this deployment thinks a
 * client may reasonably take. */
#define FPM_PCTL_RETIRE_NO_TIMEOUT_MS 30000

struct fpm_pctl_retire_s {
	pid_t pid;
	struct timeval deadline;
	int quit_sent;
};

static struct fpm_pctl_retire_s fpm_pctl_retire_tbl[FPM_PCTL_RETIRE_MAX];
static int fpm_pctl_retire_used;
static int fpm_pctl_retire_full_warned;

static struct fpm_pctl_retire_s *fpm_pctl_retire_find(pid_t pid)
{
	int i;

	for (i = 0; i < fpm_pctl_retire_used; i++) {
		if (fpm_pctl_retire_tbl[i].pid == pid) {
			return &fpm_pctl_retire_tbl[i];
		}
	}
	return NULL;
}

static void fpm_pctl_retire_deadline_in(struct fpm_pctl_retire_s *e, int ms)
{
	struct timeval now, add;

	fpm_clock_get(&now);
	add.tv_sec = ms / 1000;
	add.tv_usec = (ms % 1000) * 1000;
	timeradd(&now, &add, &e->deadline);
}

static struct fpm_pctl_retire_s *fpm_pctl_retire_add(pid_t pid, int ms)
{
	struct fpm_pctl_retire_s *e;

	if (fpm_pctl_retire_used >= FPM_PCTL_RETIRE_MAX) {
		if (!fpm_pctl_retire_full_warned) {
			fpm_pctl_retire_full_warned = 1;
			zlog(ZLOG_WARNING, "issue #166 throwaway: more than %d children are retiring at once; "
				"the rest are scaled down the unmodified way", FPM_PCTL_RETIRE_MAX);
		}
		return NULL;
	}
	e = &fpm_pctl_retire_tbl[fpm_pctl_retire_used++];
	e->pid = pid;
	e->quit_sent = 0;
	fpm_pctl_retire_deadline_in(e, ms);
	return e;
}

void fpm_pctl_retire_forget(pid_t pid)
{
	struct fpm_pctl_retire_s *e = fpm_pctl_retire_find(pid);

	if (!e) {
		return;
	}
	*e = fpm_pctl_retire_tbl[--fpm_pctl_retire_used];
}

/* Issue #166 (THROWAWAY). Unmodified FPM tells a scale-down victim to quit and
 * kills it on the next maintenance pass, one second later. For a pool type
 * whose child owns its own connections that is strategy (b) -- in-flight
 * responses finish, idle keep-alive connections are dropped with the process.
 *
 * Strategy (a) is this: ask the child to RETIRE (the per-child SIGUSR1 of
 * issue #65, which the child already answers by closing the accept socket and
 * staying alive until its live-connection count reaches zero or its own
 * http.read_timeout deadline expires), and do not escalate until that bound
 * has passed. The child's half of strategy (a) was already in main when this
 * branch was cut -- #165's measurement is what showed that the missing half
 * was here, in the master, which would not wait.
 *
 * Which types get this is data on the type (.retires_by_draining), never a
 * name comparison: a type that does not drain has nothing to wait for and
 * SIGUSR1 would mean whatever its child makes of it.
 *
 * Known hole, and acceptable for a throwaway whose question is a number: the
 * escalation below only runs on a pass that calls this function again, and the
 * caller only calls it for a child that still reads idle. A child that picked
 * up traffic again after being asked to retire would therefore keep its
 * deadline unexamined until it went idle once more. Nothing leaks in practice,
 * because the child's own drain deadline makes it exit regardless -- the
 * master's bound exists to stop the master from shooting first, not to be the
 * thing that ends the child. A merged version would have to tick the table
 * from the maintenance pass itself. */
static void fpm_pctl_kill_idle_child(struct fpm_child_s *child) /* {{{ */
{
	const struct fpm_pool_type_s *type = child->wp ? fpm_pool_type_of(child->wp) : NULL;

	if (type && type->retires_by_draining && child->wp->config) {
		struct fpm_pctl_retire_s *e = fpm_pctl_retire_find(child->pid);
		struct timeval now;

		if (!e && !child->idle_kill) {
			/* http.read_timeout = 0 means the pool has no client read timeout at
			 * all, so the child has no deadline of its own to bound this with.
			 * The master still grants one -- an unbounded wait would turn a
			 * scale-down into a leak -- and takes the directive's own default. */
			int budget = child->wp->config->http_read_timeout;
			int bound = (budget > 0 ? budget : FPM_PCTL_RETIRE_NO_TIMEOUT_MS) + FPM_PCTL_RETIRE_SLACK_MS;

			e = fpm_pctl_retire_add(child->pid, bound);
			if (e) {
				child->idle_kill = true;
				zlog(ZLOG_NOTICE, "[pool %s] child %d asked to retire and drain, "
					"escalating in %d ms at the earliest (issue #166 throwaway)",
					child->wp->config->name, (int) child->pid, bound);
				kill(child->pid, SIGUSR1);
				return;
			}
			/* Table full: fall through to the unmodified escalation. */
		}

		if (e) {
			fpm_clock_get(&now);
			if (timercmp(&now, &e->deadline, <)) {
				return;		/* inside the bound the master granted */
			}
			if (!e->quit_sent) {
				e->quit_sent = 1;
				fpm_pctl_retire_deadline_in(e, FPM_PCTL_RETIRE_QUIT_MS);
				zlog(ZLOG_WARNING, "[pool %s] child %d did not drain within its bound; "
					"sending SIGQUIT (issue #166 throwaway)",
					child->wp->config->name, (int) child->pid);
				fpm_pctl_kill(child->pid, FPM_PCTL_QUIT);
			} else {
				zlog(ZLOG_WARNING, "[pool %s] child %d survived SIGQUIT after its bound; "
					"sending SIGKILL (issue #166 throwaway)",
					child->wp->config->name, (int) child->pid);
				fpm_pctl_retire_forget(child->pid);
				fpm_pctl_kill(child->pid, FPM_PCTL_KILL);
			}
			return;
		}
	}

	if (child->idle_kill) {
		fpm_pctl_kill(child->pid, FPM_PCTL_KILL);
	} else {
		child->idle_kill = true;
		fpm_pctl_kill(child->pid, FPM_PCTL_QUIT);
	}
}
/* }}} */

static void fpm_pctl_perform_idle_server_maintenance(struct timeval *now) /* {{{ */
{
	struct fpm_worker_pool_s *wp;

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		struct fpm_child_s *child;
		struct fpm_child_s *last_idle_child = NULL;
		int idle = 0;
		int active = 0;
		int children_to_fork;
		unsigned cur_lq = 0;

		if (wp->config == NULL) continue;

		/* update status structure for all PMs */
		if (wp->listen_address_domain == FPM_AF_INET) {
			if (0 > fpm_socket_get_listening_queue(wp->listening_socket, &cur_lq, NULL)) {
				cur_lq = 0;
#if 0
			} else {
				if (cur_lq > 0) {
					if (!wp->warn_lq) {
						zlog(ZLOG_WARNING, "[pool %s] listening queue is not empty, #%d requests are waiting to be served, consider raising pm.max_children setting (%d)", wp->config->name, cur_lq, wp->config->pm_max_children);
						wp->warn_lq = 1;
					}
				} else {
					wp->warn_lq = 0;
				}
#endif
			}
		}

		fpm_scoreboard_update_begin(wp->scoreboard);

		for (child = wp->children; child; child = child->next) {
			if (fpm_request_is_idle(child)) {
				if (last_idle_child == NULL) {
					last_idle_child = child;
				} else {
					if (timercmp(&child->started, &last_idle_child->started, <)) {
						last_idle_child = child;
					}
				}
				idle++;
			} else {
				active++;
			}
		}

		fpm_scoreboard_update_commit(idle, active, cur_lq, -1, -1, -1, 0, 0, FPM_SCOREBOARD_ACTION_SET, wp->scoreboard);

		/* this is specific to PM_STYLE_ONDEMAND */
		if (wp->config->pm == PM_STYLE_ONDEMAND) {
			struct timeval last, now;

			zlog(ZLOG_DEBUG, "[pool %s] currently %d active children, %d spare children", wp->config->name, active, idle);

			if (!last_idle_child) continue;

			fpm_request_last_activity(last_idle_child, &last);
			fpm_clock_get(&now);
			if (last.tv_sec < now.tv_sec - wp->config->pm_process_idle_timeout) {
				fpm_pctl_kill_idle_child(last_idle_child);
			}

			continue;
		}

		/* the rest is only used by PM_STYLE_DYNAMIC */
		if (wp->config->pm != PM_STYLE_DYNAMIC) continue;

		zlog(ZLOG_DEBUG, "[pool %s] currently %d active children, %d spare children, %d running children. Spawning rate %d", wp->config->name, active, idle, wp->running_children, wp->idle_spawn_rate);

		if (idle > wp->config->pm_max_spare_servers && last_idle_child) {
			fpm_pctl_kill_idle_child(last_idle_child);
			wp->idle_spawn_rate = 1;
			continue;
		}

		if (idle < wp->config->pm_min_spare_servers) {
			if (wp->running_children >= wp->config->pm_max_children) {
				if (!wp->warn_max_children) {
					fpm_scoreboard_update(0, 0, 0, 0, 0, 1, 0, 0, FPM_SCOREBOARD_ACTION_INC, wp->scoreboard);
					zlog(ZLOG_WARNING, "[pool %s] server reached pm.max_children setting (%d), consider raising it", wp->config->name, wp->config->pm_max_children);
					wp->warn_max_children = 1;
				}
				wp->idle_spawn_rate = 1;
				continue;
			}

			if (wp->idle_spawn_rate >= 8) {
				zlog(ZLOG_WARNING, "[pool %s] seems busy (you may need to increase pm.start_servers, or pm.min/max_spare_servers), spawning %d children, there are %d idle, and %d total children", wp->config->name, wp->idle_spawn_rate, idle, wp->running_children);
			}

			/* compute the number of idle process to spawn */
			children_to_fork = MIN(wp->idle_spawn_rate, wp->config->pm_min_spare_servers - idle);

			/* get sure it won't exceed max_children */
			children_to_fork = MIN(children_to_fork, wp->config->pm_max_children - wp->running_children);
			if (children_to_fork <= 0) {
				if (!wp->warn_max_children) {
					fpm_scoreboard_update(0, 0, 0, 0, 0, 1, 0, 0, FPM_SCOREBOARD_ACTION_INC, wp->scoreboard);
					zlog(ZLOG_WARNING, "[pool %s] server reached pm.max_children setting (%d), consider raising it", wp->config->name, wp->config->pm_max_children);
					wp->warn_max_children = 1;
				}
				wp->idle_spawn_rate = 1;
				continue;
			}
			wp->warn_max_children = 0;

			fpm_children_make(wp, 1, children_to_fork, 1);

			/* if it's a child, stop here without creating the next event
			 * this event is reserved to the master process
			 */
			if (fpm_globals.is_child) {
				return;
			}

			zlog(ZLOG_DEBUG, "[pool %s] %d child(ren) have been created dynamically", wp->config->name, children_to_fork);

			/* Double the spawn rate for the next iteration */
			if (wp->idle_spawn_rate < wp->config->pm_max_spawn_rate) {
				wp->idle_spawn_rate *= 2;
			}
			continue;
		}
		wp->idle_spawn_rate = 1;
	}
}
/* }}} */

void fpm_pctl_heartbeat(struct fpm_event_s *ev, short which, void *arg) /* {{{ */
{
	static struct fpm_event_s heartbeat;
	struct timeval now;

	if (fpm_globals.parent_pid != getpid()) {
		return; /* sanity check */
	}

	if (which == FPM_EV_TIMEOUT) {
		fpm_clock_get(&now);
		fpm_pctl_check_request_timeout(&now);
		return;
	}

	/* ensure heartbeat is not lower than FPM_PCTL_MIN_HEARTBEAT */
	fpm_globals.heartbeat = MAX(fpm_globals.heartbeat, FPM_PCTL_MIN_HEARTBEAT);

	/* first call without setting to initialize the timer */
	zlog(ZLOG_DEBUG, "heartbeat have been set up with a timeout of %dms", fpm_globals.heartbeat);
	fpm_event_set_timer(&heartbeat, FPM_EV_PERSIST, &fpm_pctl_heartbeat, NULL);
	fpm_event_add(&heartbeat, fpm_globals.heartbeat);
}
/* }}} */

void fpm_pctl_perform_idle_server_maintenance_heartbeat(struct fpm_event_s *ev, short which, void *arg) /* {{{ */
{
	static struct fpm_event_s heartbeat;
	struct timeval now;

	if (fpm_globals.parent_pid != getpid()) {
		return; /* sanity check */
	}

	if (which == FPM_EV_TIMEOUT) {
		fpm_clock_get(&now);
		if (fpm_pctl_can_spawn_children()) {
			fpm_pctl_perform_idle_server_maintenance(&now);

			/* if it's a child, stop here without creating the next event
			 * this event is reserved to the master process
			 */
			if (fpm_globals.is_child) {
				return;
			}
		}
		return;
	}

	/* first call without setting which to initialize the timer */
	fpm_event_set_timer(&heartbeat, FPM_EV_PERSIST, &fpm_pctl_perform_idle_server_maintenance_heartbeat, NULL);
	fpm_event_add(&heartbeat, FPM_IDLE_SERVER_MAINTENANCE_HEARTBEAT);
}
/* }}} */

void fpm_pctl_on_socket_accept(struct fpm_event_s *ev, short which, void *arg) /* {{{ */
{
	struct fpm_worker_pool_s *wp = (struct fpm_worker_pool_s *)arg;
	struct fpm_child_s *child;


	if (fpm_globals.parent_pid != getpid()) {
		/* prevent a event race condition when child process
		 * have not set up its own event loop */
		return;
	}

	wp->socket_event_set = 0;

/*	zlog(ZLOG_DEBUG, "[pool %s] heartbeat running_children=%d", wp->config->name, wp->running_children);*/

	if (wp->running_children >= wp->config->pm_max_children) {
		if (!wp->warn_max_children) {
			fpm_scoreboard_update(0, 0, 0, 0, 0, 1, 0, 0, FPM_SCOREBOARD_ACTION_INC, wp->scoreboard);
			zlog(ZLOG_WARNING, "[pool %s] server reached max_children setting (%d), consider raising it", wp->config->name, wp->config->pm_max_children);
			wp->warn_max_children = 1;
		}

		return;
	}

	for (child = wp->children; child; child = child->next) {
		/* if there is at least on idle child, it will handle the connection, stop here */
		if (fpm_request_is_idle(child)) {
			return;
		}
	}
	wp->warn_max_children = 0;
	fpm_children_make(wp, 1, 1, 1);

	if (fpm_globals.is_child) {
		return;
	}

	zlog(ZLOG_DEBUG, "[pool %s] got accept without idle child available .... I forked", wp->config->name);
}
/* }}} */
