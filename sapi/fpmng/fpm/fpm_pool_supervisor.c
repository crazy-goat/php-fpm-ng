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
 * The pidfd watchdog (stop_timeout, and supervisor.max_runtime — issue #326,
 * a cap on a single script iteration) and script execution outside a FastCGI
 * request are shared with pool.type = cron — see fpm_pool_watchdog.[ch] and
 * fpm_pool_script.[ch].
 *
 * supervisor.restart_jitter/supervisor.start_jitter (issue #323): additive
 * randomization on top of the deterministic policy above, so that several
 * copies of the same pool (supervisor.processes > 1) or several supervisor
 * pools sharing a dependency do not restart or cold-start in lockstep. Both
 * default to 0 (no jitter), which is exactly today's deterministic behavior.
 * See fpm_pool_supervisor_jitter(), the restart_jitter application inside
 * fpm_pool_supervisor_apply_policy(), and the start_jitter application at the
 * top of the loop in fpm_pool_supervisor_child_main().
 *
 * supervisor.max_memory/supervisor.stop_signal (issue #324): a memory-triggered
 * recycle, the supervisor.* equivalent of pm.max_requests -- checked right
 * after fpm_pool_script_run() returns (see fpm_pool_supervisor_memory_bytes()
 * and the check in fpm_pool_supervisor_child_main()), but only AFTER
 * fpm_pool_supervisor_apply_policy() has run with the script's real exit code
 * and decided the pool is not done for good (supervisor.restart = never/
 * on-failure still park the process exactly as before -- a memory recycle
 * never overrides that decision). Given that, it is a healthy recycle, not a
 * failure, in the one sense that matters: apply_policy() already treats
 * exit_code == 0 as "no failure" regardless of memory, so the common case
 * (a script that keeps completing normally but has grown too big) consumes
 * neither supervisor.restart_max nor supervisor.restart_delay. A script that
 * both fails AND is over budget still counts as a real failure -- the memory
 * limit recycles the process either way, it does not launder a failing exit
 * code. supervisor.stop_signal generalizes the signal used to ask the current
 * script to stop cleanly (default SIGTERM, today's behavior) -- reused for
 * both an external termination request and a memory-triggered recycle, so
 * both give the script the same stop_timeout grace period before SIGKILL.
 */

#include "fpm_config.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
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
#include "fpm_pool_output_log.h"
#include "fpm_cleanup.h"
#include "fpm_shm.h"
#include "fpm_children.h"
#include "fpm_children_extra.h"
#include "fpm_events.h"
#include "fpm_debug_clock.h"
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
	"worker.",
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

	/* Fields added solely for the operator status page (docs/NOTES.md 3u) —
	 * exactly what that page shows, not one field more. "failures"/"terminal"/
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

	/* issue #323: how many of the pool's supervisor_processes cold-start jitter
	 * slots have already been handed out. Bumped at the moment a process
	 * decides to apply (or skip, because the roll was 0) start_jitter, not
	 * after its script has run -- a process that crashes immediately afterwards
	 * still consumed its slot. Without this, a process respawned after an early
	 * crash would re-enter fpm_pool_supervisor_child_main() with a fresh
	 * is_first_iteration and, so long as some slower sibling had kept "starts"
	 * (above) under supervisor_processes, would be granted a SECOND cold-start
	 * delay on top of the backoff it had already served -- exactly what
	 * start_jitter's docs promise will not happen. Shared, not local, for the
	 * same reason "starts" is: the fact "this pool has already spent N of its
	 * cold-start slots" must survive the child dying and being respawned. */
	unsigned long cold_starts_issued;

	/* issue #323: the DETERMINISTIC part of the delay the most recent failure
	 * computed (restart_delay, doubled per consecutive failure, capped at
	 * restart_delay_max) -- excluding any jitter. next_allowed_start above is
	 * pool-wide (every copy of this pool shares the same shared memory, so
	 * whichever copy fails most recently overwrites it for all of them,
	 * exactly like "failures" already does); if restart_jitter's random
	 * component were folded into that single shared value the way the
	 * deterministic part is, every copy would still wake at the identical
	 * instant -- last jitter draw wins, same lockstep the directive exists to
	 * break. So jitter is NOT stored here: each copy draws its OWN jitter,
	 * independently, at the moment it is about to wait (see the top of the
	 * loop in fpm_pool_supervisor_child_main()), and adds it on top of the
	 * shared deterministic wait. This field is what a percentage-form jitter
	 * needs to know "a percentage of what" without recomputing the backoff
	 * doubling itself. */
	unsigned long last_deterministic_delay;

	/* Issue #122: the fast-restart warning. With restart = always an exit 0 is
	 * the end of one work unit and the next one starts at once -- the script
	 * sets the pace, deliberately (see the exit_code == 0 branch below). A
	 * script that returns instead of looping therefore spins a whole core with
	 * nothing in the log; measured at 12086 runs per second. These three fields
	 * buy a single WARNING for that case and change no behaviour.
	 *
	 * Shared rather than local for the same reason as "starts": the streak must
	 * survive this child dying and being respawned, which is one of the two ways
	 * the script starts again. */
	unsigned long fast_runs;		/* consecutive runs shorter than FPM_SUPERVISOR_FAST_RUN_MS */
	unsigned long fast_started_ms;		/* monotonic ms at the start of the current streak */
	unsigned char fast_warned;		/* 1 = already warned about this streak */

	/* fpmng_supervisor_heartbeat() (issue #327). Purely observational, exactly
	 * like last_start/last_exit_code above: nothing in this file ever reads
	 * last_heartbeat back to decide anything about restart/backoff policy.
	 * Epoch of the most recent call, 0 = the script has never called it in this
	 * pool's lifetime (not "in the current process" -- shared memory survives a
	 * respawn, same reasoning as `starts`). What the status page derives from
	 * it (an "age" since the last call) is computed where it is rendered, not
	 * stored here, so a clock step does not have to be reconciled against a
	 * cached duration. */
	time_t last_heartbeat;
};

/* A run this short did no useful work of its own: it is one PHP startup and
 * shutdown and little else. Deliberately not a directive -- it is the point at
 * which a message is worth printing, not a policy anyone should tune. */
#define FPM_SUPERVISOR_FAST_RUN_MS 5

/* And this many of them in a row before saying anything. High enough that a
 * script legitimately doing short bursts of work is never warned about, low
 * enough that an operator mistake is reported within a second of starting. */
#define FPM_SUPERVISOR_FAST_RUN_STREAK 1000

/* Issue #329: how long a reload survivor (see fpm_pool_supervisor_reload_spare_child()
 * below) is allowed to sit unsignalled, waiting for this generation's
 * replacement to confirm it started, before this pool retires it
 * unconditionally. Not tied to process_control_timeout -- that directive
 * defaults to 0 ("escalate the reload's own kill fan-out immediately"), which
 * would retire the survivor before its replacement ever got a chance, the
 * opposite of what it exists for. A fixed constant instead: generous enough to
 * cover fork + exec + PHP bootstrap + a configured supervisor.start_jitter of
 * ordinary size, bounded enough that a broken new generation (bad script,
 * crash loop) does not leave the survivor running indefinitely. A pool whose
 * supervisor.start_jitter is configured well past this is a documented edge
 * case (docs/supervisor.md) rather than something this reacts to dynamically. */
#define FPM_SUPERVISOR_RELOAD_SURVIVOR_TIMEOUT_S 30

/* How often the retirement check below polls this generation's shared->starts.
 * Small enough that the survivor is retired promptly once a replacement is
 * confirmed (the whole point is to keep the OLD generation's process count
 * from outliving the NEW one's first success by any meaningful amount), cheap
 * enough that polling it costs nothing worth measuring. */
#define FPM_SUPERVISOR_RELOAD_SURVIVOR_POLL_MS 200

/* State for ONE child of ONE pool that a reload spared from the kill fan-out
 * (fpm_pool_supervisor_reload_spare_child()) and that THIS generation
 * (fpm_pool_supervisor_init_main(), after execvp()) is now watching for
 * retirement. `child` is the detached struct this generation never forked --
 * fpm_children_detach_oldest() handed it to the OLD generation, which passed
 * only its pid across the execvp() (see the env var in
 * fpm_pool_supervisor_reload_spare_child()); this generation reconstructs a
 * fresh struct fpm_child_s around that bare pid purely as the vehicle
 * fpm_children_free() expects at retirement time -- see
 * fpm_pool_supervisor_reload_survivor_track(). */
struct fpm_supervisor_reload_survivor_s {
	struct fpm_worker_pool_s *wp;
	struct fpm_child_s *child;
	struct fpm_supervisor_shared_s *new_shared;	/* this generation's shared struct */
	struct fpm_event_s poll_ev;
	time_t deadline;				/* epoch; retire unconditionally past this */
};

struct fpm_supervisor_registry_s {
	struct fpm_worker_pool_s *wp;
	struct fpm_supervisor_shared_s *shared;
	struct fpm_supervisor_reload_survivor_s *reload_survivor;	/* issue #329; NULL = none pending */
	struct fpm_supervisor_registry_s *next;
};

static struct fpm_supervisor_registry_s *supervisor_registry = NULL;
static int supervisor_cleanup_registered = 0;

/* This process's own shared state, set once by fpm_pool_supervisor_child_main()
 * before the loop starts. fpmng_supervisor_heartbeat() (below) needs it and has
 * no `wp` to look it up with -- it is a PHP-callable function, called from deep
 * inside the script with no argument of its own (issue #327 asks for exactly
 * that shape, mirroring fpm_metric_*()). A process-local static rather than a
 * registry walk keyed by getpid(): each supervisor CHILD process is its own
 * process image with its own copy of this variable, so there is no cross-child
 * ambiguity to resolve in the first place. */
static struct fpm_supervisor_shared_s *supervisor_current_shared = NULL;

/* fpmng_supervisor_heartbeat() -- issue #327. A long-running supervisor script
 * that does not naturally return between units of work (the case the fast-run
 * warning above assumes is rare) has otherwise no way to distinguish "still
 * working" from "stuck" on the status page, which only ever shows when the
 * CURRENT iteration started. Calling this periodically records a liveness
 * timestamp; fpm_pool_supervisor_status() exposes it and the age since it, and
 * that is the entire feature -- no restart, no kill, no backoff reacts to a
 * stale or a missing heartbeat (see the file-level comment: purely
 * observational, no new control behaviour).
 *
 * Returns false, doing nothing, when there is no current supervisor context to
 * record into -- the same "nothing to report" convention fpm_connection_info()
 * uses (fpm_http_direct.c). This is deliberately a runtime guard rather than
 * relying only on "the function is not registered outside a supervisor child"
 * (see fpm_pool_supervisor_register_heartbeat_builtin() below): a script that
 * saved a callable reference before some future refactor, or is run through a
 * code path this file cannot see, gets a safe false instead of a NULL
 * dereference. */
static ZEND_FUNCTION(fpmng_supervisor_heartbeat)
{
	ZEND_PARSE_PARAMETERS_NONE();

	if (!supervisor_current_shared) {
		RETURN_FALSE;
	}
	supervisor_current_shared->last_heartbeat = FPM_NOW();
	RETURN_TRUE;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fpmng_supervisor_heartbeat, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry fpm_supervisor_functions[] = {
	ZEND_FE(fpmng_supervisor_heartbeat, arginfo_fpmng_supervisor_heartbeat)
	ZEND_FE_END
};

/* MODULE_TEMPORARY for the same reason fpm_http_direct.c's
 * fpm_direct_register_functions() documents at length (and fpm_acme_challenge.c
 * before it): zend_register_functions() stamps EG(current_module) into every
 * entry, MODULE_PERSISTENT would let opcache's function_exists() folding bake
 * "this function exists" into an SHM entry keyed only on the script path, which
 * is wrong for something registered per fork in one pool type only. */
static zend_module_entry fpm_supervisor_module_entry = {
	.size = sizeof(zend_module_entry),
	.zend_api = ZEND_MODULE_API_NO,
	.zend_debug = ZEND_DEBUG,
	.zts = USING_ZTS,
	.name = "fpmng_supervisor_builtins",
	.type = MODULE_TEMPORARY,
	.build_id = ZEND_MODULE_BUILD_ID,
};

/* Registered once per supervisor child, mirroring fpm_direct_register_functions()
 * (fpm_http_direct.c): the function exists ONLY in a process running this pool
 * type, so `function_exists('fpmng_supervisor_heartbeat')` is false everywhere
 * else -- a cron pool, an http/http-direct worker, a plain FastCGI child -- the
 * same way fpm_connection_info() is absent outside http-direct. That is the
 * "non-supervisor pool context" case issue #327 asks to be safe; the runtime
 * guard in the function body above covers the rest (a supervisor child calling
 * it before/after this registration could, in principle, reach it through some
 * other path). */
static void fpm_pool_supervisor_register_heartbeat_builtin(const char *pool_name)
{
	zend_module_entry *saved_module = EG(current_module);
	zend_result result;

	EG(current_module) = &fpm_supervisor_module_entry;
	result = zend_register_functions(NULL, fpm_supervisor_functions, CG(function_table), MODULE_PERSISTENT);
	EG(current_module) = saved_module;
	if (result != SUCCESS) {
		zlog(ZLOG_ERROR, "[pool %s] supervisor: cannot register fpmng_supervisor_heartbeat()", pool_name);
	}
}

static volatile sig_atomic_t supervisor_term_requested = 0;
static volatile sig_atomic_t supervisor_stop_timeout = 10;

/* Handler for supervisor.stop_signal (issue #324; SIGTERM before that, always).
 * Installed for whichever signal number that directive resolves to -- the name
 * stays "sigterm" in spirit only: this is the ONE stop signal this pool reacts
 * to, whatever it is configured as. */
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
		fpm_pool_watchdog_arm(getpid(), (unsigned) supervisor_stop_timeout, SIGKILL);
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
	/* issue #323: negative values are refused the same way fpm_conf_set_time()
	 * lets a leading '-' through today (see its comment in fpm_conf.c) --
	 * clamped here rather than made an outright config error so this matches
	 * every other delay/timeout field above instead of being the one
	 * directive that is stricter about it. */
	if (c->supervisor_restart_jitter < 0) {
		c->supervisor_restart_jitter = 0;
	}
	if (c->supervisor_restart_jitter_percent < 0) {
		c->supervisor_restart_jitter_percent = 0;
	} else if (c->supervisor_restart_jitter_percent > 100) {
		c->supervisor_restart_jitter_percent = 100;
	}
	if (c->supervisor_start_jitter < 0) {
		c->supervisor_start_jitter = 0;
	}
	if (c->supervisor_stop_signal == 0) {
		/* Belt and braces alongside the default set at config-struct allocation
		 * time (fpm_conf_alloc()): sigaction(0, ...) is not a valid call, and
		 * this is the one place every pool's config is guaranteed to pass
		 * through before use. */
		c->supervisor_stop_signal = SIGTERM;
	}
	if (c->supervisor_max_runtime < 0) {
		c->supervisor_max_runtime = 0;
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

/* Issue #329: the single env var a reload's OLD generation uses to hand a
 * spared child's pid to the NEW one across execvp() -- shared memory cannot
 * do this (fpm_shm_alloc()'s MAP_ANONYMOUS mapping does not survive
 * execve(), see fpm_pool_type.h's reload_spare_child doc comment), but
 * execvp() keeps the calling process's environment, which is exactly the
 * "surviving state" this needs and nothing more.
 *
 * One var for every pool rather than one var per pool (which would need
 * turning a pool name into an env-var-safe identifier, and de-duplicating
 * that against another pool's sanitized name): "name:pid" pairs,
 * comma-separated. fpm-ng pool names are the section header between '['
 * and ']' in the config (fpm_conf.c, untouched) and in every configuration
 * this project ships or tests contain neither ':' nor ',' -- a name that did
 * would collide with this format, so fpm_pool_supervisor_reload_spare_child()
 * below refuses to spare a child for such a pool rather than risk producing
 * an unparsable var (see its own comment). */
#define FPM_SUPERVISOR_RELOAD_SURVIVOR_ENV "FPMNG_RELOAD_SURVIVORS"

/* Appends "wp->config->name:pid" to FPM_SUPERVISOR_RELOAD_SURVIVOR_ENV's
 * current value (creating it if unset). Master only, called right after
 * detaching the child that pid belongs to. */
static void fpm_pool_supervisor_reload_survivor_env_add(const char *name, pid_t pid) /* {{{ */
{
	const char *existing = getenv(FPM_SUPERVISOR_RELOAD_SURVIVOR_ENV);
	size_t len = (existing ? strlen(existing) : 0) + strlen(name) + 32;
	char *next = malloc(len);

	if (!next) {
		return; /* best-effort: worst case this pool's reload is not staggered this time */
	}
	if (existing && *existing) {
		snprintf(next, len, "%s,%s:%d", existing, name, (int) pid);
	} else {
		snprintf(next, len, "%s:%d", name, (int) pid);
	}
	setenv(FPM_SUPERVISOR_RELOAD_SURVIVOR_ENV, next, 1);
	free(next);
}
/* }}} */

/* Finds and removes THIS pool's "name:pid" pair from
 * FPM_SUPERVISOR_RELOAD_SURVIVOR_ENV, returning the pid or 0 if there was
 * none. Removes it (rewrites the var without it) rather than leaving it in
 * place so a LATER reload -- one this pool's own execvp() chain lives through
 * without ever restarting the whole binary from init(1) -- does not read a
 * pid some unrelated process has been recycled into by then; this generation
 * consumes its own entry once, here, during its own init_main(). */
static pid_t fpm_pool_supervisor_reload_survivor_env_take(const char *name) /* {{{ */
{
	const char *existing = getenv(FPM_SUPERVISOR_RELOAD_SURVIVOR_ENV);
	char *copy, *rest, *kept;
	size_t name_len = strlen(name);
	pid_t found = 0;

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
		char *comma = strchr(rest, ',');
		char *pair = rest;

		if (comma) {
			*comma = '\0';
			rest = comma + 1;
		} else {
			rest = NULL;
		}

		if (!found && strncmp(pair, name, name_len) == 0 && pair[name_len] == ':') {
			found = (pid_t) strtol(pair + name_len + 1, NULL, 10);
			continue; /* drop this pair from `kept` */
		}

		if (*pair) {
			if (*kept) {
				strcat(kept, ",");
			}
			strcat(kept, pair);
		}
	}

	if (found > 0) {
		if (*kept) {
			setenv(FPM_SUPERVISOR_RELOAD_SURVIVOR_ENV, kept, 1);
		} else {
			unsetenv(FPM_SUPERVISOR_RELOAD_SURVIVOR_ENV);
		}
	}

	free(copy);
	free(kept);
	return found;
}
/* }}} */

/* fpm_pool_type_s.reload_spare_child -- issue #329. Called from
 * fpm_pctl_kill_all() (fpm_process_ctl.c) in the OLD generation, once per
 * pool, only on a reload's first signal pass. See the field's doc comment in
 * fpm_pool_type.h for the full contract. */
void fpm_pool_supervisor_reload_spare_child(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_child_s *spared;
	struct fpm_supervisor_registry_s *e;

	/* No spare capacity: with exactly one copy, detaching it would leave the
	 * script with zero running copies for the ENTIRE reload instead of just
	 * the tail of it -- worse, not better. supervisor.processes == 1 keeps
	 * today's behavior (a real gap; see docs/supervisor.md). */
	if (wp->running_children < 2) {
		return;
	}

	/* A reload landing before THIS generation's own reload_survivor (spared by
	 * the reload before this one) has retired: FPM_SUPERVISOR_RELOAD_SURVIVOR_ENV
	 * holds one "name:pid" pair per pool, and fpm_pool_supervisor_reload_survivor_env_take()
	 * only ever consumes the first match for a given name -- a second pair
	 * appended here would sit in the env var forever, unconsumed, leaking its
	 * process past even the 30s safety timeout (which only bounds a survivor
	 * that IS being tracked). Simplest safe answer: skip sparing this time and
	 * take the ordinary brief reload gap instead -- back-to-back reloads faster
	 * than one confirmed restart apart are rare, and this pool has already
	 * spared a child once very recently. */
	for (e = supervisor_registry; e; e = e->next) {
		if (e->wp == wp) {
			if (e->reload_survivor) {
				zlog(ZLOG_NOTICE, "[pool %s] supervisor: a previous reload's spared child is still "
					"being retired; not sparing another one for this reload (issue #329)",
					wp->config->name);
				return;
			}
			break;
		}
	}

	if (strchr(wp->config->name, ':') || strchr(wp->config->name, ',')) {
		zlog(ZLOG_DEBUG, "[pool %s] supervisor: pool name contains ':' or ',', "
			"cannot stage a reload survivor handoff for it (issue #329) -- reloading without one",
			wp->config->name);
		return;
	}

	spared = fpm_children_detach_oldest(wp);
	if (!spared) {
		return;
	}

	fpm_pool_supervisor_reload_survivor_env_add(wp->config->name, spared->pid);

	zlog(ZLOG_NOTICE, "[pool %s] supervisor: sparing child %d from this reload's signal -- "
		"it keeps running the current generation's script until a replacement copy is confirmed "
		"running after execvp() (issue #329)",
		wp->config->name, (int) spared->pid);

	/* Reaping it (whenever that happens -- deliberate retirement after
	 * execvp(), or it exits on its own for any of the usual supervisor
	 * reasons while still spared) must not fall into fpm_children_bury()'s
	 * "unknown child" branch, and this OLD generation is about to exec away
	 * without ever finding out either way -- so there is nothing useful this
	 * process itself could do with the notification. Forgetting it here
	 * (rather than watching it) is deliberate: fpm_children_extra's registry
	 * is this process's own heap, which the execvp() below discards exactly
	 * like everything else non-shared; the NEW generation re-establishes its
	 * own watch on this pid via fpm_pool_supervisor_reload_survivor_track()
	 * once it reads the pid back out of the env var above. */
}
/* }}} */

static void fpm_pool_supervisor_reload_survivor_free(struct fpm_supervisor_reload_survivor_s *surv) /* {{{ */
{
	struct fpm_supervisor_registry_s *e;

	for (e = supervisor_registry; e; e = e->next) {
		if (e->wp == surv->wp) {
			e->reload_survivor = NULL;
			break;
		}
	}

	surv->child->next = NULL;
	fpm_children_free(surv->child);
	free(surv);
}
/* }}} */

/* fpm_children_extra_watch()'s on_exit: the survivor has actually exited,
 * whether from retire() below asking it to (replacement confirmed, or the
 * safety timeout) or on its own (crash, supervisor.max_memory/max_runtime
 * recycling it, restart=never/on-failure parking it -- all of which apply to
 * it completely normally: it is an ordinary already-running supervisor child
 * that simply has not been signalled by THIS reload yet). Either way the pid
 * is gone and must not be signalled again -- fpm_event_del() first,
 * unconditionally, so a timer tick already queued cannot fire kill() against
 * a pid the OS may since have reused for something unrelated. */
static void fpm_pool_supervisor_reload_survivor_exited(void *arg, pid_t pid, int status) /* {{{ */
{
	struct fpm_supervisor_reload_survivor_s *surv = arg;

	(void) status;
	fpm_event_del(&surv->poll_ev);
	zlog(ZLOG_DEBUG, "[pool %s] supervisor: reload survivor pid %d reaped",
		surv->wp->config->name, (int) pid);
	fpm_pool_supervisor_reload_survivor_free(surv);
}
/* }}} */

static void fpm_pool_supervisor_reload_survivor_retire(struct fpm_supervisor_reload_survivor_s *surv, const char *why) /* {{{ */
{
	zlog(ZLOG_NOTICE, "[pool %s] supervisor: retiring reload survivor pid %d (%s)",
		surv->wp->config->name, (int) surv->child->pid, why);
	fpm_event_del(&surv->poll_ev);
	kill(surv->child->pid, surv->wp->config->supervisor_stop_signal);
	/* Do not free surv/surv->child here -- the process has only been ASKED to
	 * exit. fpm_children_bury() reaps the real exit later and calls
	 * fpm_pool_supervisor_reload_survivor_exited() above, which frees it. */
}
/* }}} */

static void fpm_pool_supervisor_reload_survivor_poll(struct fpm_event_s *ev, short which, void *arg) /* {{{ */
{
	struct fpm_supervisor_reload_survivor_s *surv = arg;

	(void) ev;
	if (which != FPM_EV_TIMEOUT) {
		return;
	}

	/* shared->starts is bumped at the top of this generation's loop, before
	 * the script itself runs (see the for(;;) below) -- so this is true the
	 * moment a replacement copy has committed to running, not only once it
	 * has completed anything, which matters for a script that is meant to
	 * exit almost immediately. */
	if (surv->new_shared->starts >= 1) {
		fpm_pool_supervisor_reload_survivor_retire(surv, "replacement confirmed running");
		return;
	}

	if (FPM_NOW() >= surv->deadline) {
		zlog(ZLOG_WARNING, "[pool %s] supervisor: no replacement copy had started %ds after reload; "
			"retiring reload survivor pid %d unconditionally to avoid an indefinite orphan",
			surv->wp->config->name, FPM_SUPERVISOR_RELOAD_SURVIVOR_TIMEOUT_S, (int) surv->child->pid);
		fpm_pool_supervisor_reload_survivor_retire(surv, "safety timeout");
		return;
	}

	/* Still waiting: FPM_EV_PERSIST re-arms this timer on its own (see
	 * fpm_tls_reload_master_tick() for the same idiom), nothing to do here. */
}
/* }}} */

/* Called once from fpm_pool_supervisor_init_main() (the NEW generation, right
 * after execvp()), for a pool whose entry in FPM_SUPERVISOR_RELOAD_SURVIVOR_ENV
 * named a still-alive pid. Registers the retirement poll and the
 * fpm_children_extra watch that reaps it whichever way it eventually exits. */
static void fpm_pool_supervisor_reload_survivor_track(struct fpm_worker_pool_s *wp,
		struct fpm_supervisor_registry_s *entry, pid_t pid) /* {{{ */
{
	struct fpm_supervisor_reload_survivor_s *surv = calloc(1, sizeof(*surv));
	struct fpm_child_s *child;

	if (!surv) {
		zlog(ZLOG_WARNING, "[pool %s] supervisor: cannot track reload survivor pid %d, "
			"retiring it immediately instead", wp->config->name, (int) pid);
		kill(pid, wp->config->supervisor_stop_signal);
		return;
	}

	/* This generation never forked `pid` -- it is a bare number carried
	 * across execvp() in an env var (see fpm_pool_supervisor_reload_spare_child()).
	 * A minimal struct fpm_child_s built here is purely the shape
	 * fpm_children_free()/fpm_child_close() expect at retirement time; fd_stdout
	 * and fd_stderr are -1 (this generation owns no pipe to it -- the OLD
	 * generation's master-side forwarding for it ended when THAT master
	 * execvp()'d away, which is also why any output the survivor produces
	 * between being spared and being retired no longer reaches error_log via
	 * master-side forwarding; supervisor.output_log, issue #328, is unaffected,
	 * since that fd belongs to the child process itself, not the master). */
	child = calloc(1, sizeof(*child));
	if (!child) {
		free(surv);
		kill(pid, wp->config->supervisor_stop_signal);
		return;
	}
	child->pid = pid;
	child->fd_stdout = -1;
	child->fd_stderr = -1;

	surv->wp = wp;
	surv->child = child;
	surv->new_shared = entry->shared;
	surv->deadline = FPM_NOW() + FPM_SUPERVISOR_RELOAD_SURVIVOR_TIMEOUT_S;

	entry->reload_survivor = surv;

	fpm_children_extra_watch(pid, fpm_pool_supervisor_reload_survivor_exited, surv);

	fpm_event_set_timer(&surv->poll_ev, FPM_EV_PERSIST, fpm_pool_supervisor_reload_survivor_poll, surv);
	fpm_event_add(&surv->poll_ev, FPM_SUPERVISOR_RELOAD_SURVIVOR_POLL_MS);

	zlog(ZLOG_NOTICE, "[pool %s] supervisor: reload survivor pid %d is still running the previous "
		"generation's script; watching for this generation's first start to retire it (issue #329)",
		wp->config->name, (int) pid);
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

	/* Issue #329: init_main() runs in every generation, including the very
	 * first one this master ever starts (no FPM_SUPERVISOR_RELOAD_SURVIVOR_ENV
	 * set at all -- take() below is then a no-op returning 0) as well as after
	 * every execvp()-triggered reload. If the OLD generation spared a child of
	 * THIS pool, its pid is in the env var now; take() both finds it and
	 * removes it so a later reload doesn't see a stale/reused pid. */
	{
		pid_t survivor_pid = fpm_pool_supervisor_reload_survivor_env_take(wp->config->name);

		if (survivor_pid > 0) {
			if (0 == kill(survivor_pid, 0)) {
				fpm_pool_supervisor_reload_survivor_track(wp, entry, survivor_pid);
			} else {
				zlog(ZLOG_DEBUG, "[pool %s] supervisor: reload survivor pid %d from the previous "
					"generation is already gone", wp->config->name, (int) survivor_pid);
			}
		}
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
		FPM_SLEEP(1);
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

/* Monotonic milliseconds. CLOCK_MONOTONIC rather than time(NULL) because the
 * question here is "how long did that take", at a resolution time(NULL) does
 * not have, and a clock the operator can step must not be able to invent or
 * erase a streak. */
static unsigned long fpm_pool_supervisor_now_ms(void) /* {{{ */
{
	struct timespec ts;

	if (FPM_MONOTONIC(&ts) != 0) {
		return 0;
	}
	return (unsigned long) ts.tv_sec * 1000UL + (unsigned long) (ts.tv_nsec / 1000000L);
}
/* }}} */

/* A fresh value in [0, max_value] (inclusive), max_value == 0 returns 0 without
 * touching the clock. Deliberately NOT libc's rand()/srand(): that state is
 * process-wide and SURVIVES fork() -- every one of a pool's supervisor.processes
 * copies is forked from the SAME master, so a "seed once, whoever gets there
 * first" guard would let every child inherit the identical sequence position
 * the master had at fork time and produce the identical "random" delay, which
 * silently collapses back into the exact lockstep restart/cold-start issue #323
 * exists to break. Hashing a monotonic clock reading together with the pid
 * instead has no shared state to inherit: two calls a nanosecond apart, in any
 * process, already differ. FNV-1a for the mixing step, chosen only because it
 * is a few dependency-free lines with reasonable distribution for a delay in
 * the tens-of-seconds range -- no cryptographic property is needed here. */
static unsigned fpm_pool_supervisor_jitter(unsigned max_value) /* {{{ */
{
	struct timespec ts;
	unsigned long hash = 2166136261UL;
	unsigned long mix;
	size_t i;

	if (max_value == 0) {
		return 0;
	}

	/* Matches fpm_pool_supervisor_now_ms()'s handling of the same call: on the
	 * (practically unreachable, but ts is otherwise read uninitialized)
	 * failure of clock_gettime(), fall back to an all-zero reading rather than
	 * mixing in garbage stack contents -- getpid() alone still keeps two
	 * concurrently-forked siblings from hashing to the same value.
	 *
	 * Deliberately clock_gettime() and not FPM_MONOTONIC() (issue #396): this
	 * reading is hash entropy, not a measurement, so scaling it would only
	 * narrow the input range. */
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		ts.tv_sec = 0;
		ts.tv_nsec = 0;
	}
	mix = (unsigned long) ts.tv_sec ^ ((unsigned long) ts.tv_nsec << 1) ^ (unsigned long) getpid();
	for (i = 0; i < sizeof(mix); i++) {
		hash ^= (unsigned char) (mix >> (i * 8));
		hash *= 16777619UL;
	}
	return (unsigned) (hash % ((unsigned long) max_value + 1));
}
/* }}} */

/* Backoff and the "should we try again?" decision, applied between subsequent
 * script executions in the SAME process and also by every fresh respawn after a
 * crash (because shared memory survives process death). */
static void fpm_pool_supervisor_apply_policy(struct fpm_worker_pool_s *wp,
		struct fpm_supervisor_shared_s *shared, int exit_code, time_t duration,
		unsigned long duration_ms) /* {{{ */
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

		/* Issue #122: that contract is the right one, and it is also indis-
		 * tinguishable from a script that was meant to loop and returns instead.
		 * The decision was to say so once and change nothing: no floor, no new
		 * directive, no delay. An operator who meant it loses one log line; an
		 * operator who did not gets told which of the two mistakes it is. */
		if (duration_ms < FPM_SUPERVISOR_FAST_RUN_MS) {
			unsigned long now_ms = fpm_pool_supervisor_now_ms();

			if (shared->fast_runs == 0) {
				shared->fast_started_ms = now_ms;
			}
			shared->fast_runs++;

			if (shared->fast_runs >= FPM_SUPERVISOR_FAST_RUN_STREAK && !shared->fast_warned) {
				unsigned long elapsed_ms = now_ms - shared->fast_started_ms;
				unsigned long per_second = elapsed_ms > 0
					? shared->fast_runs * 1000UL / elapsed_ms
					: shared->fast_runs;

				shared->fast_warned = 1;
				zlog(ZLOG_WARNING, "[pool %s] supervisor: %lu consecutive runs of '%s' finished in under %dms each "
					"(about %lu restarts per second, one core spent on PHP startup and shutdown). "
					"supervisor.restart = always restarts on exit 0 at once, by design -- the script sets the pace. "
					"If it was meant to keep running, it is returning early; if it was meant to run once, "
					"set supervisor.restart = never. See docs/supervisor.md (issue #122)",
					c->name, shared->fast_runs, c->supervisor_script,
					FPM_SUPERVISOR_FAST_RUN_MS, per_second);
			}
		} else {
			/* One run that did some work re-arms the warning: the next streak is
			 * a new fact about the pool, not a repeat of the one already reported. */
			shared->fast_runs = 0;
			shared->fast_warned = 0;
		}
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

		/* issue #323: restart_jitter is deliberately NOT added here. This
		 * delay, like "failures" above, is pool-wide shared state -- every
		 * copy of this pool reads the SAME shared->next_allowed_start, so
		 * whichever copy's jitter draw was folded in last would apply to all
		 * of them, and every copy would wake at that one identical instant:
		 * exactly the lockstep restart_jitter exists to break. Instead, only
		 * the deterministic delay is stored (both in next_allowed_start and,
		 * for the percentage form, in last_deterministic_delay so a waiter
		 * can compute "N% of it" later); each copy draws its own jitter
		 * independently at the moment it is about to wait, in
		 * fpm_pool_supervisor_child_main(). */
		shared->last_deterministic_delay = (unsigned long) delay;
		shared->next_allowed_start = FPM_NOW() + delay;
		zlog(ZLOG_NOTICE, "[pool %s] supervisor: script exited (code %d) after %lds, restarting in %lds"
			"%s (failure %u%s)",
			c->name, exit_code, (long) duration, (long) delay,
			(c->supervisor_restart_jitter > 0 || c->supervisor_restart_jitter_is_percent) ? " + this copy's own jitter" : "",
			shared->failures, c->supervisor_restart_max > 0 ? "" : "/unlimited");
	}
}
/* }}} */

/* This process's own peak resident set size, in bytes, for supervisor.max_memory
 * (issue #324). getrusage(RUSAGE_SELF).ru_maxrss is a monotonically
 * non-decreasing high-water mark -- exactly what a one-way "has this process
 * outgrown its budget" check needs, and unlike /proc/self/status it is
 * portable to non-Linux dev builds. The one thing it is NOT portable about is
 * its unit: Linux (the target platform, .github/workflows/build-matrix.yml
 * runs ubuntu-latest) reports kilobytes; Darwin/macOS (a local dev build,
 * never the target container) reports bytes. Both are converted to bytes here
 * so supervisor.max_memory (parsed by fpm_conf_set_bytes(), also bytes) never
 * has to know which platform it is running on. */
static size_t fpm_pool_supervisor_memory_bytes(void) /* {{{ */
{
	struct rusage ru;

	if (getrusage(RUSAGE_SELF, &ru) != 0) {
		return 0;
	}
#ifdef __APPLE__
	return (size_t) ru.ru_maxrss;
#else
	return (size_t) ru.ru_maxrss * 1024;
#endif
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
	supervisor_current_shared = shared;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fpm_pool_supervisor_sigterm;
	sigemptyset(&sa.sa_mask);
	/* SIGTERM is ALWAYS handled, regardless of supervisor.stop_signal:
	 * fpm_pctl_kill_all() (fpm_process_ctl.c, reference code, untouched) sends
	 * every non-request-serving pool a hardcoded SIGTERM on a graceful master
	 * shutdown (see its comment: "supervisor, cron: each has its own SIGTERM
	 * handler") -- an operator has no way to change what the MASTER sends, so
	 * losing this handler whenever stop_signal != TERM would silently turn
	 * `docker stop`/systemd shutdown into an unhandled kill with no
	 * stop_timeout grace period at all. */
	sigaction(SIGTERM, &sa, NULL);
	/* issue #324: supervisor.stop_signal (default SIGTERM, in which case this
	 * is a harmless repeat of the sigaction() above) is what THIS pool sends
	 * to itself when IT decides to recycle (memory-triggered today) and what a
	 * script may pcntl_signal() a trap onto for the SAME grace period as an
	 * external SIGTERM gets — additive to, never instead of, the SIGTERM
	 * handler above. */
	if (c->supervisor_stop_signal != SIGTERM) {
		sigaction(c->supervisor_stop_signal, &sa, NULL);
	}

	fpm_pool_script_install_sapi_overrides();
	/* issue #328: once per process, before the loop below and before anything
	 * might write to stdout/stderr -- see fpm_pool_output_log.h. A no-op when
	 * supervisor.output_log is not set. Also a respawn after a park() below
	 * gets its own fresh call: this is a NEW process, with its own
	 * fpm_stdio_init_child() having already set stdout/stderr to whatever
	 * catch_workers_output/dev-null default it uses, and that has to be
	 * overridden again exactly like the sigaction() calls above are redone on
	 * every respawn. */
	fpm_pool_output_log_redirect(c->name, c->supervisor_output_log);
	/* Once per process, like fpm_direct_register_functions() -- CG(function_table)
	 * outlives a request, and this pool runs its script many times over. Before
	 * the loop, not inside fpm_pool_script_run(), for the same reason as
	 * fpm_pool_script_register_acme_builtins(): a script that calls it from its
	 * very first line must already find it there. */
	fpm_pool_supervisor_register_heartbeat_builtin(c->name);

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

	/* issue #323: supervisor.start_jitter spreads the fork+exec+bootstrap cost
	 * of a COLD start across a pool's supervisor.processes copies, instead of
	 * paying all of it in the same instant every one of them is forked at
	 * master startup. Whether THIS process still owes a cold start is decided
	 * from shared->cold_starts_issued, a pool-wide counter of how many of the
	 * supervisor_processes cold-start slots have been handed out so far (see
	 * its declaration for why it is bumped at decision time, not after the
	 * script runs) -- not from shared->starts, which also counts restarts and
	 * so cannot tell a genuine cold start apart from a crash-respawn racing a
	 * slow sibling's first start. is_first_iteration additionally confines the
	 * CHECK to once per process (this process's first trip through the loop):
	 * without it, a process whose script keeps exiting instantly would retry
	 * the "do I still owe a cold start" question every iteration instead of
	 * only its very first. */
	int is_first_iteration = 1;

	for (;;) {
		time_t now, started, duration;
		unsigned long started_ms, duration_ms;
		int exit_code;
		pid_t max_runtime_watchdog = -1;

		if (supervisor_term_requested) {
			break;
		}

		now = FPM_NOW();
		if (shared->next_allowed_start > now) {
			time_t wait_for = shared->next_allowed_start - now;

			/* issue #323: restart_jitter is drawn HERE, independently by
			 * each copy about to wait, rather than once in apply_policy()
			 * and folded into the shared next_allowed_start -- see the
			 * comment on shared->last_deterministic_delay for why: a value
			 * shared by every copy of this pool can only hold one random
			 * draw, and whichever copy's failure wrote it last would apply
			 * to all of them, collapsing straight back into the lockstep
			 * this directive exists to break. */
			if (c->supervisor_restart_jitter > 0 || c->supervisor_restart_jitter_is_percent) {
				unsigned jitter_max = c->supervisor_restart_jitter_is_percent
					? (unsigned) ((shared->last_deterministic_delay * (unsigned long) c->supervisor_restart_jitter_percent) / 100UL)
					: (unsigned) c->supervisor_restart_jitter;

				wait_for += (time_t) fpm_pool_supervisor_jitter(jitter_max);
			}

			fpm_pool_supervisor_wait(wait_for);
			if (supervisor_term_requested) {
				break;
			}
			/* A sibling copy may have exhausted supervisor_restart_max (or
			 * finished under restart = never) while this one slept -- without
			 * this check it would run its script anyway, even after
			 * supervisor.fatal already asked the master to shut down. */
			if (shared->terminal) {
				break;
			}
		}

		if (is_first_iteration) {
			is_first_iteration = 0;
			if (c->supervisor_start_jitter > 0 &&
					shared->cold_starts_issued < (unsigned long) (c->supervisor_processes > 0 ? c->supervisor_processes : 1)) {
				unsigned delay;

				shared->cold_starts_issued++;
				delay = fpm_pool_supervisor_jitter((unsigned) c->supervisor_start_jitter);

				if (delay > 0) {
					fpm_pool_supervisor_wait((time_t) delay);
					if (supervisor_term_requested) {
						break;
					}
					if (shared->terminal) {
						break;
					}
				}
			}
		}

		started = FPM_NOW();
		started_ms = fpm_pool_supervisor_now_ms();
		shared->starts++;
		shared->last_start = started;
		shared->running = 1;

		/* supervisor.max_runtime (issue #326): caps a SINGLE iteration, the
		 * supervisor.* equivalent of cron.timeout -- but unlike cron (one script
		 * run per process, see fpm_pool_cron_child_main()), THIS process does not
		 * end between iterations, so the watchdog armed for one iteration must be
		 * explicitly canceled once that iteration returns on time; otherwise it
		 * would keep counting from the WRONG start time and could fire during a
		 * later, well-behaved iteration (see fpm_pool_watchdog.h). signo =
		 * supervisor.stop_signal (issue #324), not SIGKILL directly: firing
		 * re-enters fpm_pool_supervisor_sigterm() below through the same handler
		 * already installed for external termination, which arms its OWN SIGKILL
		 * watchdog for supervisor_stop_timeout -- exactly the same two-stage
		 * "stop_signal, then hard SIGKILL" an external `docker stop` already
		 * gets, reused rather than reinvented. */
		if (c->supervisor_max_runtime > 0) {
			max_runtime_watchdog = fpm_pool_watchdog_arm(getpid(), (unsigned) c->supervisor_max_runtime, c->supervisor_stop_signal);
		}

		exit_code = fpm_pool_script_run(c->name, c->supervisor_script, c->supervisor_stop_signal);

		if (max_runtime_watchdog > 0) {
			/* The iteration returned -- on time or because it caught SIGTERM and
			 * exited on its own -- before the watchdog's timer fired. Cancel it:
			 * kill() first (it may already have exited quietly on its own if the
			 * timer JUST fired, in which case this is a harmless ESRCH), then
			 * waitpid() to reap it rather than leaving a zombie behind for every
			 * iteration this pool ever runs. Retry on EINTR -- our own SIGTERM
			 * handler (sa_flags = 0, no SA_RESTART) can otherwise cut this wait
			 * short and leave that one watchdog unreaped; harmless (the loop
			 * breaks right after on a real stop), but not reaping it if a
			 * signal just happens to land here costs nothing to avoid. */
			kill(max_runtime_watchdog, SIGKILL);
			while (waitpid(max_runtime_watchdog, NULL, 0) < 0 && errno == EINTR) {
				continue;
			}
			max_runtime_watchdog = -1;
		}

		shared->running = 0;
		shared->last_exit_code = exit_code;
		shared->has_last_exit_code = 1;
		duration = FPM_NOW() - started;
		duration_ms = fpm_pool_supervisor_now_ms() - started_ms;

		/* issue #324: apply_policy() runs FIRST, with the script's REAL exit
		 * code -- max_memory must recycle the process without disturbing that
		 * decision, not skip it. apply_policy() already treats exit_code == 0
		 * as "not a failure" (resets the counter, no restart_delay, no
		 * restart_max accounting) regardless of memory, which is exactly the
		 * "not counted as a failure" contract this directive promises for the
		 * common case; a script that both FAILS and is over budget still counts
		 * as a real failure here, on purpose (supervisor.restart_max means "this
		 * many actual failures", not "this many total recycles" -- see
		 * docs/supervisor.md). Checking max_memory only when apply_policy() did
		 * NOT set shared->terminal also means supervisor.restart = never/
		 * on-failure keep their contract: a script this pool has decided not to
		 * run again parks (fpm_pool_supervisor_park() at the top of this
		 * function on the next respawn) instead of being kept alive by a memory
		 * recycle that exits and gets it immediately restarted regardless of
		 * the configured policy.
		 *
		 * issue #326: unlike max_memory above, a supervisor.max_runtime kill is
		 * deliberately NOT exempted from this same accounting -- see
		 * docs/supervisor.md "supervisor.max_runtime" for the reasoning.
		 * apply_policy() below runs unmodified with whatever exit_code the
		 * script actually returned (including one set by a script that trapped
		 * the max_runtime stop signal and exited on its own). */
		fpm_pool_supervisor_apply_policy(wp, shared, exit_code, duration, duration_ms);

		if (shared->terminal) {
			break;
		}

		/* The self-signal below cannot interrupt anything
		 * (fpm_pool_script_run() has already fully returned by this point,
		 * including php_request_shutdown()) -- it exists so a memory-triggered
		 * recycle arms the exact same stop_timeout watchdog as any other
		 * recycle and so "continue" below reaches the loop's own
		 * "if (supervisor_term_requested) break" through the one path that
		 * already exists for it, instead of a second, parallel exit route. */
		if (c->supervisor_max_memory > 0) {
			size_t memory_bytes = fpm_pool_supervisor_memory_bytes();

			if (memory_bytes >= c->supervisor_max_memory) {
				zlog(ZLOG_NOTICE, "[pool %s] supervisor: memory usage %zu bytes reached "
					"supervisor.max_memory = %zu bytes, recycling (not counted as a failure)",
					c->name, memory_bytes, c->supervisor_max_memory);
				kill(getpid(), c->supervisor_stop_signal);
				continue;
			}
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
		 * in the master before anything can fork, including the operator
		 * endpoint's child, which is what calls this. A zero state is a safe
		 * result. */
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

	/* fpmng_supervisor_heartbeat() (issue #327): has_heartbeat is "the script
	 * has ever called it", not "recently" -- a renderer computes the age
	 * itself against time(NULL), exactly like uptime/backoff_seconds above,
	 * rather than this file deciding what counts as stuck. */
	if (shared->last_heartbeat != 0) {
		out->has_heartbeat = 1;
		out->last_heartbeat = shared->last_heartbeat;
	}
}
/* }}} */
