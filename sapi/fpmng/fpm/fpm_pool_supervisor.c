/* fpm-ng: pool.type = supervisor.
 *
 * See fpm_pool_supervisor.h and docs/NOTES.md for the design rationale. In
 * short: supervisor.processes maps to pm = static + pm.max_children, so process
 * spawning and resurrection are entirely handled by fpm_children.c (untouched,
 * as required by the contract in NOTES.md 3h). What fpm_children.c CANNOT do is
 * HOLD BACK a resurrection (it respawns immediately and unconditionally) —
 * therefore the restart/backoff/restart_max/fatal policy lives in shared memory
 * per pool and is checked BY THE CHILD at startup and between script executions,
 * rather than by changing fpm_children.c.
 *
 * The pidfd watchdog (stop_timeout) and script execution outside a FastCGI
 * request are shared with pool.type = cron — see fpm_pool_watchdog.[ch] and
 * fpm_pool_script.[ch].
 */

#include "fpm_config.h"

#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "php.h"

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_supervisor.h"
#include "fpm_pool_type.h"
#include "fpm_pool_watchdog.h"
#include "fpm_pool_script.h"
#include "fpm_cleanup.h"
#include "fpm_shm.h"
#include "zlog.h"

/* Rejected, not allowed, directives — see fpm_pool_type_check_directives().
 * A name ending in a dot matches the whole family (for example, "pm." matches
 * pm.max_children, pm.start_servers, ...).
 *
 * "pm" and "pm." are rejected entirely: supervisor.processes IS pm.max_children
 * under another name (see fpm_pool_supervisor_validate), so allowing the user to
 * set pm.* as well would create two sources of truth for the same number.
 * "listen" and "listen." are rejected entirely: this type listens to nothing.
 * "ping." and "access." are also rejected entirely — without listen there is
 * nothing to ping or log as "access". */
const char *const fpm_pool_supervisor_rejects[] = {
	"listen",
	"listen.",
	"pm",
	"pm.",
	"request_terminate_timeout",
	"request_terminate_timeout_track_finished",
	"request_slowlog_timeout",
	"request_slowlog_trace_depth",
	"slowlog",
	"ping.",
	"access.",
	"security.limit_extensions",
	"http.",
	"fiber.",
	NULL
};

/* State shared by ALL processes of this pool (so it survives both a respawn
 * after a crash and subsequent "iterations" in the same process). */
struct fpm_supervisor_shared_s {
	unsigned failures;			/* consecutive "fast" deaths/failures */
	time_t next_allowed_start;		/* epoch; 0 or past = may start immediately */
	unsigned char terminal;			/* 1 = policy says "no more attempts" */
	unsigned char gave_up;			/* 1 = terminal because restart_max was exhausted
						 * (a real failure — see supervisor.fatal),
						 * 0 = terminal because restart=never/on-failure succeeded
						 * (planned completion, NOT a failure) */
	unsigned char fatal_signaled;		/* SIGTERM sent to the master once already (idempotent) */

	/* Fields added solely for pool.type = status (docs/NOTES.md 3u) — exactly
	 * what status actually shows, not one field more. "failures"/"terminal"/
	 * "gave_up" above already existed and serve both policy and status; the three
	 * below serve status ONLY, and the policy does not read them. */
	unsigned char running;			/* 1 = the script is currently running */
	time_t last_start;			/* epoch start of the last iteration, 0 = none yet */
	int last_exit_code;			/* exit code of the last COMPLETED iteration */
	unsigned char has_last_exit_code;

	/* Issue #277: every start of the supervised script, by every process of
	 * this pool. Monotonic, and in the shared region rather than in the loop's
	 * own stack because there are two ways to start again and both count -- the
	 * loop below going round, and this child dying and the master respawning
	 * it, which loses any counter the child was keeping.
	 *
	 * Starts, not restarts, even though the reported counter is restarts: what
	 * makes a start a restart is that it is not one of the pool's first, and
	 * "the pool's first" is supervisor.processes of them, one per process. The
	 * subtraction is done where the number is read (fpm_pool_supervisor_status)
	 * so that this field stays a plain count of an event that happened. */
	unsigned long starts;
};

struct fpm_supervisor_registry_s {
	struct fpm_worker_pool_s *wp;
	struct fpm_supervisor_shared_s *shared;
	struct fpm_supervisor_registry_s *next;
};

static struct fpm_supervisor_registry_s *supervisor_registry = NULL;
static int supervisor_cleanup_registered = 0;

static volatile sig_atomic_t supervisor_term_requested = 0;
static volatile sig_atomic_t supervisor_stop_timeout = 10;

static void fpm_pool_supervisor_sigterm(int signo)
{
	(void) signo;
	if (!supervisor_term_requested) {
		supervisor_term_requested = 1;

		/* Safety net: if the current iteration (a script that checks nothing
		 * between its own steps) does not finish within stop_timeout, kill
		 * ourselves with KILL.
		 *
		 * Deliberately do NOT use alarm()/SIGALRM here: PHP itself uses SIGALRM
		 * (or SIGPROF, depending on the build) for its own max_execution_time
		 * (zend_set_timeout_ex(), Zend/zend_execute_API.c), and under
		 * ZEND_SIGNALS reinstalls that handler on EVERY script execution — our raw
		 * sigaction(SIGALRM,...) would be silently replaced and would fire into
		 * Zend's handler instead of ours (measured: this project builds with
		 * -DZEND_SIGNALS). Instead, fork a tiny watchdog process
		 * (fpm_pool_watchdog_arm(), shared with pool.type = cron — see
		 * fpm_pool_watchdog.h), completely independent of PHP's signal state: it
		 * waits for stop_timeout and, if we (the supervisor process) are still
		 * alive, kills us. Safe here in the handler — see the comment in
		 * fpm_pool_watchdog.h. */
		fpm_pool_watchdog_arm(getpid(), (unsigned) supervisor_stop_timeout);
	}
}

/* }}} signals */

int fpm_pool_supervisor_validate(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_worker_pool_config_s *c = wp->config;

	if (!c->supervisor_script || !*c->supervisor_script) {
		zlog(ZLOG_ALERT, "[pool %s] pool.type = supervisor requires supervisor.script", c->name);
		return -1;
	}

	if (!c->supervisor_restart || !*c->supervisor_restart) {
		free(c->supervisor_restart);
		c->supervisor_restart = strdup("always");
		if (!c->supervisor_restart) {
			return -1;
		}
	} else if (strcmp(c->supervisor_restart, "always") != 0 &&
			strcmp(c->supervisor_restart, "on-failure") != 0 &&
			strcmp(c->supervisor_restart, "never") != 0) {
		zlog(ZLOG_ALERT, "[pool %s] supervisor.restart must be 'always', 'on-failure' or 'never', got '%s'",
			c->name, c->supervisor_restart);
		return -1;
	}

	if (c->supervisor_processes < 1) {
		c->supervisor_processes = 1;
	}
	if (c->supervisor_restart_delay < 1) {
		c->supervisor_restart_delay = 1;
	}
	if (c->supervisor_restart_delay_max < c->supervisor_restart_delay) {
		c->supervisor_restart_delay_max = c->supervisor_restart_delay;
	}
	if (c->supervisor_stop_timeout < 1) {
		c->supervisor_stop_timeout = 10;
	}
	if (c->supervisor_restart_max < 0) {
		c->supervisor_restart_max = 0;
	}

	/* Design decision (see NOTES.md): supervisor.processes maps to pm = static +
	 * pm.max_children so spawning/resurrecting N processes comes for free from
	 * the existing fpm_children.c machinery. The user does not set pm.* (rejected
	 * by the rejects list above), so there are no two conflicting sources of truth. */
	c->pm = PM_STYLE_STATIC;
	c->pm_max_children = c->supervisor_processes;

	return 0;
}
/* }}} */

static void fpm_pool_supervisor_exit_main(int which, void *arg) /* {{{ */
{
	struct fpm_supervisor_registry_s *e;

	(void) which;
	(void) arg;

	/* Called from FPM_CLEANUP_PARENT_EXIT_MAIN, just before exit(FPM_EXIT_OK)
	 * in fpm_pctl_exit() (fpm_process_ctl.c, untouched). If any supervisor pool
	 * gave up because of a real failure and has supervisor.fatal=yes, terminate
	 * the entire process here with a non-zero code — otherwise fpm_pctl_exit()
	 * would finish with code 0 anyway. */
	for (e = supervisor_registry; e; e = e->next) {
		if (e->shared->gave_up && e->wp->config->supervisor_fatal) {
			zlog(ZLOG_ALERT, "[pool %s] supervisor.fatal: master is going down with a non-zero exit code",
				e->wp->config->name);
			_exit(FPM_EXIT_SOFTWARE);
		}
	}
}
/* }}} */

int fpm_pool_supervisor_init_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_supervisor_registry_s *entry;
	struct fpm_supervisor_shared_s *shared = fpm_shm_alloc(sizeof(*shared));

	if (!shared) {
		zlog(ZLOG_ERROR, "[pool %s] supervisor: cannot allocate shared memory", wp->config->name);
		return -1;
	}

	/* MEASURED (docs/NOTES.md, "graceful stopping", scenario 3): when SIGTERM
	 * goes to the MASTER (exactly what `docker stop`/systemd sends, without any
	 * additional STOPSIGNAL configuration), the master enters TERMINATING and
	 * escalates on its own through fpm_process_ctl.c (reference code, untouched)
	 * — this is behavior of the entire FPM master, not something introduced by
	 * this pool type. The default process_control_timeout = 0 escalates to SIGKILL
	 * almost immediately, so supervisor.stop_timeout NEVER gets a chance to work:
	 * the process dies from the master's SIGKILL before our own watchdog even
	 * starts counting. This cannot be fixed in this file
	 * (process_control_timeout is global, shared by all pools, and
	 * fpm_process_ctl.c is reference code) — but we CAN warn the operator loudly,
	 * once, at startup, instead of leaving them with a quiet "works on my test"
	 * (where SIGTERM goes DIRECTLY to the child, not the master) and a
	 * `docker stop` that fails in production. */
	if (fpm_global_config.process_control_timeout < wp->config->supervisor_stop_timeout) {
		zlog(ZLOG_WARNING,
			"[pool %s] supervisor.stop_timeout = %ds but global process_control_timeout = %ds; "
			"SIGTERM/SIGQUIT sent to the MASTER (e.g. `docker stop`) kills this child through the "
			"master's escalation before supervisor.stop_timeout can act — set process_control_timeout "
			">= %ds in [global] if SIGTERM/docker stop should give this pool time to finish its job",
			wp->config->name, wp->config->supervisor_stop_timeout, fpm_global_config.process_control_timeout,
			wp->config->supervisor_stop_timeout);
	}

	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		return -1;
	}
	entry->wp = wp;
	entry->shared = shared;
	entry->next = supervisor_registry;
	supervisor_registry = entry;

	if (!supervisor_cleanup_registered) {
		if (0 > fpm_cleanup_add(FPM_CLEANUP_PARENT_EXIT_MAIN, fpm_pool_supervisor_exit_main, 0)) {
			return -1;
		}
		supervisor_cleanup_registered = 1;
	}

	return 0;
}
/* }}} */

static struct fpm_supervisor_shared_s *fpm_pool_supervisor_shared_for(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_supervisor_registry_s *e;

	for (e = supervisor_registry; e; e = e->next) {
		if (e->wp == wp) {
			return e->shared;
		}
	}
	return NULL;
}
/* }}} */

static void fpm_pool_supervisor_wait(time_t seconds) /* {{{ */
{
	while (seconds-- > 0 && !supervisor_term_requested) {
		sleep(1);
	}
}
/* }}} */

static void fpm_pool_supervisor_park(void) /* {{{ */
{
	/* This process is a respawn after another process of this pool already
	 * decided "finished". Do nothing and stay alive — if we called exit()
	 * immediately, fpm_children.c would resurrect us in an endless loop. Sleeping
	 * until the first signal is cheap and harmless. */
	while (!supervisor_term_requested) {
		pause();
	}
}
/* }}} */

/* Backoff and the "should we try again?" decision, applied between subsequent
 * script executions in the SAME process and also by every fresh respawn after a
 * crash (because shared memory survives process death). */
static void fpm_pool_supervisor_apply_policy(struct fpm_worker_pool_s *wp,
		struct fpm_supervisor_shared_s *shared, int exit_code, time_t duration) /* {{{ */
{
	struct fpm_worker_pool_config_s *c = wp->config;
	int wants_retry;

	if (!strcmp(c->supervisor_restart, "never")) {
		wants_retry = 0;
	} else if (!strcmp(c->supervisor_restart, "on-failure")) {
		wants_retry = (exit_code != 0);
	} else {
		wants_retry = 1;
	}

	if (!wants_retry) {
		shared->terminal = 1;
		shared->gave_up = 0;
		shared->failures = 0;
		zlog(ZLOG_NOTICE, "[pool %s] supervisor: script finished (exit code %d), restart = %s -> not restarting",
			c->name, exit_code, c->supervisor_restart);
		return;
	}

	if (exit_code == 0) {
		/* Success: with restart=always this is the normal, expected end of one
		 * "work unit" (the script controls its own pace, for example with its own
		 * sleep()), NOT a failure — reset the counter and start the next iteration
		 * immediately, without artificial throttling on our side. */
		shared->failures = 0;
		shared->next_allowed_start = 0;
		return;
	}

	/* From here on: a real failure (exit != 0). "Ran long enough" before the
	 * failure resets the counter — otherwise one unlucky restart after weeks of
	 * operation would count toward the same restart_max as a genuine fast
	 * crash-loop. Threshold: restart_delay_max, the same value at which backoff
	 * would otherwise plateau. */
	if (duration >= (time_t) c->supervisor_restart_delay_max) {
		shared->failures = 0;
	}
	shared->failures++;

	if (c->supervisor_restart_max > 0 && shared->failures >= (unsigned) c->supervisor_restart_max) {
		shared->terminal = 1;
		shared->gave_up = 1;
		zlog(ZLOG_ALERT, "[pool %s] supervisor: %u consecutive failures, giving up (supervisor.restart_max = %d)",
			c->name, shared->failures, c->supervisor_restart_max);
	} else {
		time_t delay = c->supervisor_restart_delay;
		unsigned i;

		for (i = 1; i < shared->failures; i++) {
			if (delay >= c->supervisor_restart_delay_max) {
				delay = c->supervisor_restart_delay_max;
				break;
			}
			delay *= 2;
		}
		if (delay > c->supervisor_restart_delay_max) {
			delay = c->supervisor_restart_delay_max;
		}
		shared->next_allowed_start = time(NULL) + delay;
		zlog(ZLOG_NOTICE, "[pool %s] supervisor: script exited (code %d) after %lds, restarting in %lds (failure %u%s)",
			c->name, exit_code, (long) duration, (long) delay, shared->failures,
			c->supervisor_restart_max > 0 ? "" : "/unlimited");
	}
}
/* }}} */

void fpm_pool_supervisor_child_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_worker_pool_config_s *c = wp->config;
	struct fpm_supervisor_shared_s *shared = fpm_pool_supervisor_shared_for(wp);
	struct sigaction sa;

	if (!shared) {
		/* Should not happen — init_main allocates this for every supervisor pool
		 * before anything forks. Without this state there is no safe way to
		 * calculate backoff/restart_max, so refuse to run. */
		zlog(ZLOG_ERROR, "[pool %s] supervisor: no shared state, refusing to run", c->name);
		exit(FPM_EXIT_SOFTWARE);
	}

	supervisor_stop_timeout = c->supervisor_stop_timeout;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fpm_pool_supervisor_sigterm;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);

	fpm_pool_script_install_sapi_overrides();

	if (shared->terminal) {
		/* fpm_children.c respawned us after a previous process of this pool had
		 * already decided "finished". See fpm_pool_supervisor_park(). */
		if (shared->gave_up && c->supervisor_fatal && !shared->fatal_signaled) {
			shared->fatal_signaled = 1;
			zlog(ZLOG_ALERT, "[pool %s] supervisor.fatal: asking the master to shut down", c->name);
			kill(fpm_globals.parent_pid, SIGTERM);
		}
		fpm_pool_supervisor_park();
		exit(shared->gave_up ? FPM_EXIT_SOFTWARE : FPM_EXIT_OK);
	}

	for (;;) {
		time_t now, started, duration;
		int exit_code;

		if (supervisor_term_requested) {
			break;
		}

		now = time(NULL);
		if (shared->next_allowed_start > now) {
			fpm_pool_supervisor_wait(shared->next_allowed_start - now);
			if (supervisor_term_requested) {
				break;
			}
		}

		started = time(NULL);
		shared->starts++;
		shared->last_start = started;
		shared->running = 1;
		exit_code = fpm_pool_script_run(c->name, c->supervisor_script);
		shared->running = 0;
		shared->last_exit_code = exit_code;
		shared->has_last_exit_code = 1;
		duration = time(NULL) - started;

		fpm_pool_supervisor_apply_policy(wp, shared, exit_code, duration);

		if (shared->terminal) {
			break;
		}
	}

	if (shared->terminal && shared->gave_up && c->supervisor_fatal && !shared->fatal_signaled) {
		shared->fatal_signaled = 1;
		zlog(ZLOG_ALERT, "[pool %s] supervisor.fatal: asking the master to shut down", c->name);
		kill(fpm_globals.parent_pid, SIGTERM);
	}

	exit(shared->terminal && shared->gave_up ? FPM_EXIT_SOFTWARE : FPM_EXIT_OK);
}
/* }}} */

void fpm_pool_supervisor_status(struct fpm_worker_pool_s *wp, struct fpm_pool_status_s *out) /* {{{ */
{
	struct fpm_supervisor_shared_s *shared = fpm_pool_supervisor_shared_for(wp);

	memset(out, 0, sizeof(*out));

	if (!shared) {
		/* Should not happen — init_main allocates this for every supervisor pool
		 * in the master before anything can fork (including the status pool). A
		 * zero state is a safe result. */
		return;
	}

	if (shared->running) {
		out->state = FPM_POOL_STATE_RUNNING;
	} else if (shared->terminal) {
		out->state = shared->gave_up ? FPM_POOL_STATE_GAVE_UP : FPM_POOL_STATE_FINISHED;
	} else {
		out->state = FPM_POOL_STATE_BACKOFF;
	}

	out->last_start = shared->last_start;
	out->last_exit_code = shared->last_exit_code;
	out->has_last_exit_code = shared->has_last_exit_code;
	out->consecutive_failures = shared->failures;
	/* Every start past the first one per process is a restart -- see
	 * fpm_supervisor_shared_s.starts. Clamped rather than allowed to go
	 * negative: during startup the pool's processes have not all started their
	 * script yet, and "no restarts yet" is the truth then. */
	{
		unsigned long expected = wp->config->supervisor_processes > 0
			? (unsigned long) wp->config->supervisor_processes : 1;

		out->baseline = shared->starts > expected ? shared->starts - expected : 0;
	}
	out->has_backoff_until = 1;
	out->backoff_until = shared->next_allowed_start;
	/* next_run is not marked as available — a scheduled due time makes sense
	 * only for cron. */
}
/* }}} */
