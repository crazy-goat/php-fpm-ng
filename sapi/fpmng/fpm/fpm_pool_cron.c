/* fpm-ng: pool.type = cron.
 *
 * See fpm_pool_cron.h and docs/NOTES.md for the design rationale.
 *
 * KEY SIMPLIFYING DECISION: no timers on the master side. After starting, a
 * child of this pool:
 *   1. calculates the next due time from the schedule (fpm_cron_schedule_next()),
 *   2. sleeps until then, interruptibly — SIGTERM wakes it and it exits cleanly,
 *   3. runs the script ONCE,
 *   4. exits.
 * FPM respawns it through the existing machinery (pm = static,
 * pm.max_children = 1, fpm_children.c respawns unconditionally and immediately,
 * UNTOUCHED); the new process calculates the next due time from "now" and sleeps
 * again. This means we do NOT touch fpm_children.c or fpm_events.c, exactly as
 * supervisor does.
 *
 * A non-obvious consequence of this decision, recorded explicitly: cron has NO
 * control state in shared memory. Minimal historical state (last start/result)
 * exists only for pool.type = status and never affects cron behavior. Every new
 * process calculates its due time ONLY from the current clock and schedule,
 * never from what its predecessor did. This is also why "overlapping runs" is
 * not a policy that needs to be written: with pm.max_children = 1, a second
 * process of this pool physically does not exist until the first finishes
 * (exit()) — fpm_children.c starts the next one only AFTER the previous one dies.
 * There is therefore no run with which another could overlap.
 *
 * TIME: UTC by default (fpm_cron_schedule_next() uses gmtime_r()).
 *
 * cron.timezone (task 033a, 2026-09-07): optional IANA zone name (for example,
 * "Europe/Warsaw"). When set, fpm_cron_schedule_next() interprets schedule
 * fields in that zone's local time (localtime_r() instead of gmtime_r()),
 * temporarily replacing TZ in the process environment and restoring it
 * immediately afterwards (see fpm_cron_schedule.c — safe ONLY because both a
 * cron-pool child and the master's event-loop thread calculate one schedule at
 * a time, never concurrently).
 * The DST transition behavior is INTENTIONAL, not a bug: a time skipped by DST
 * (spring-forward) never matches any UTC instant, so that run simply does not
 * occur; a time repeated by DST (fall-back) may match twice on the same day.
 * This is EXACTLY the behavior of ordinary cron running in local time — an
 * accepted once-a-year imperfection, not something this function tries to fix.
 * Without cron.timezone (the default), nothing changes: UTC, no DST, no such
 * trade-off.
 *
 * We do NOT CATCH UP missed runs. fpm_cron_schedule_next() always calculates
 * "what is nearest in the future from now", never "what did I miss since I last
 * ran" — if the master was off for an hour, the next run is the nearest future
 * time, not twelve overdue runs. Someone may want to add that later — it will be
 * a design change, not a fix. Task 033b (2026-09-07) settled this as a permanent
 * limitation, not a defect to fix — see docs/cron.md. An operator relying on cron
 * for something time-sensitive (a backup, an expiration) must read this BEFORE
 * configuring the pool, not discover it afterwards.
 *
 * cron.log (task 033c, 2026-09-07): optional file path to which every run
 * appends one line (start, exit code, duration) — see fpm_pool_cron_child_main().
 * This is run history, which the shared memory below deliberately does NOT keep
 * (see the comment next to fpm_cron_shared_s) — appending to a file needs no
 * control state and works even across master restarts.
 *
 * cron.timeout uses EXACTLY the same mechanism as supervisor.stop_timeout
 * (fpm_pool_watchdog_arm(), factored into fpm_pool_watchdog.c) — pidfd, watchdog
 * fork, SIGKILL after the limit. Script execution uses the same machinery as
 * supervisor (fpm_pool_script.c) — no SG(request_info) from FastCGI; sapi_module
 * overrides are safe for the same reason (this process never returns to the
 * accept loop).
 */

#include "fpm_config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "php.h"

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_cron.h"
#include "fpm_pool_type.h"
#include "fpm_cron_schedule.h"
#include "fpm_payload_dist.h"
#include "fpm_pool_watchdog.h"
#include "fpm_pool_script.h"
#include "fpm_shm.h"
#include "zlog.h"

/* Rejected, not allowed, directives — see fpm_pool_type_check_directives()
 * in fpm_pool_type.c. The same reasons as for supervisor (see
 * fpm_pool_supervisor.c): "pm"/"pm." are rejected entirely because
 * pm.max_children is generated programmatically (always 1), and allowing the
 * user to set it too would create two sources of truth; "listen"/"listen."
 * because this type listens to nothing; "ping."/"access." because without
 * listen there is nothing to ping or log as "access"; request directives
 * (request_terminate_timeout, request_slowlog_*, slowlog,
 * security.limit_extensions) because there are no FastCGI requests; and
 * "supervisor." because those directives belong to the OTHER pool type. */
const char *const fpm_pool_cron_rejects[] = {
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
	"supervisor.",
	"http.",
	"fiber.",
	NULL
};

/* State read ONLY by pool.type = status (docs/NOTES.md 3u). Unlike supervisor,
 * cron still has no policy that reads this back — every new process calculates
 * the due time only from the current clock and schedule (see the comment at the
 * top of the file), regardless of what is stored here. Exactly three fields,
 * as many as status actually shows: next_run is NOT stored here because it can
 * be calculated at any time from c->cron_parsed_schedule + time(NULL), without
 * any state — see fpm_pool_cron_status(). */
struct fpm_cron_shared_s {
	unsigned char running;		/* 1 = the script is currently running */
	time_t last_run;		/* epoch start of the last run, 0 = none yet */
	int last_exit_code;		/* exit code of the last COMPLETED run */
	unsigned char has_last_exit_code;
	unsigned consecutive_failures;	/* consecutive exit_code != 0; affects nothing,
					 * it is only a signal for a human/monitoring */
};

struct fpm_cron_registry_s {
	struct fpm_worker_pool_s *wp;
	struct fpm_cron_shared_s *shared;
	struct fpm_cron_registry_s *next;
};

static struct fpm_cron_registry_s *cron_registry = NULL;

static struct fpm_cron_shared_s *fpm_pool_cron_shared_for(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_cron_registry_s *e;

	for (e = cron_registry; e; e = e->next) {
		if (e->wp == wp) {
			return e->shared;
		}
	}
	return NULL;
}
/* }}} */

static volatile sig_atomic_t cron_term_requested = 0;

static void fpm_pool_cron_sigterm(int signo) /* {{{ */
{
	(void) signo;
	/* Only a flag — wakes the sleep() below. Unlike supervisor, do not arm a
	 * watchdog here: there is no "current iteration that may fail to finish by
	 * itself" to supervise during SLEEP (nothing is executing), and while the
	 * script is RUNNING the limit is cron.timeout (armed separately, see
	 * fpm_pool_cron_run()), not SIGTERM. Normal master escalation
	 * (process_control_timeout, see docs/NOTES.md 3p) applies to this type just
	 * as it does to every other type. */
	cron_term_requested = 1;
}
/* }}} */

int fpm_pool_cron_validate(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_worker_pool_config_s *c = wp->config;
	struct fpm_cron_schedule_s *parsed;
	char err[256];

	if (!c->cron_schedule || !*c->cron_schedule) {
		zlog(ZLOG_ALERT, "[pool %s] pool.type = cron requires cron.schedule", c->name);
		return -1;
	}
	if (!c->cron_script || !*c->cron_script) {
		zlog(ZLOG_ALERT, "[pool %s] pool.type = cron requires cron.script", c->name);
		return -1;
	}
	if (fpm_payload_dist_is_path(c->cron_script)) {
		/* Issue #171, acceptance criterion 3. An embedded script is checked
		 * here, in the master, against the payload this binary actually
		 * carries: a build that embedded nothing, or a name that is not in the
		 * archive, is a configuration error like any other and has to be one at
		 * startup. On disk the same check is deliberately NOT made -- a path
		 * may legitimately appear before the first run (fpm_pool_script.c) --
		 * but the contents of this binary cannot change while it runs. */
		const char *why = NULL;

		if (0 > fpm_payload_dist_validate(c->cron_script, &why)) {
			zlog(ZLOG_ALERT, "[pool %s] cron.script '%s': %s", c->name, c->cron_script, why);
			return -1;
		}
	}
	if (c->cron_timeout < 0) {
		c->cron_timeout = 0;
	}

	/* Bad schedule = reject the configuration NOW, at startup, with a clear
	 * message — never silently and never "approximately" at runtime. */
	parsed = calloc(1, sizeof(*parsed));
	if (!parsed) {
		zlog(ZLOG_ERROR, "[pool %s] cron: out of memory parsing schedule", c->name);
		return -1;
	}
	if (0 > fpm_cron_schedule_parse(c->cron_schedule, parsed, err, sizeof(err))) {
		zlog(ZLOG_ALERT, "[pool %s] cron.schedule '%s' is invalid: %s", c->name, c->cron_schedule, err);
		free(parsed);
		return -1;
	}
	free(c->cron_parsed_schedule);
	c->cron_parsed_schedule = parsed;

	/* cron.timezone: reject an unknown zone NOW, just like the invalid
	 * cron.schedule above, instead of silently falling back to UTC at runtime.
	 * tzset()/localtime_r() do not report an error for an unknown zone name (they
	 * simply behave as for UTC), so the only way to reject a typo with a clear
	 * message is to check that the zoneinfo data file exists (the same database
	 * localtime_r() would use anyway). */
	if (c->cron_timezone && *c->cron_timezone) {
		const char *tzdir = getenv("TZDIR");
		char path[512];

		if ((size_t) snprintf(path, sizeof(path), "%s/%s",
				tzdir ? tzdir : "/usr/share/zoneinfo", c->cron_timezone) >= sizeof(path)
				|| access(path, R_OK) != 0) {
			zlog(ZLOG_ALERT, "[pool %s] cron.timezone '%s' is not a known IANA zone name (checked '%s')",
				c->name, c->cron_timezone, path);
			return -1;
		}
	}

	/* Design decision (see NOTES.md and fpm_pool_cron.h): cron maps to
	 * pm = static + pm.max_children = 1, ALWAYS 1 — there is no "number of
	 * instances" directive like supervisor.processes because overlapping runs
	 * must be impossible BY CONSTRUCTION, not by policy. The user does not set
	 * pm.* (rejected by the rejects list above). */
	c->pm = PM_STYLE_STATIC;
	c->pm_max_children = 1;

	return 0;
}
/* }}} */

int fpm_pool_cron_init_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_cron_registry_s *entry;
	struct fpm_cron_shared_s *shared = fpm_shm_alloc(sizeof(*shared));

	if (!shared) {
		zlog(ZLOG_ERROR, "[pool %s] cron: cannot allocate shared memory", wp->config->name);
		return -1;
	}

	/* See the identical comment in fpm_pool_supervisor_init_main() — cron.timeout
	 * has exactly the same problem with SIGTERM to the MASTER: the global
	 * master's process_control_timeout escalates to SIGKILL before cron.timeout
	 * can do anything. */
	if (fpm_global_config.process_control_timeout < wp->config->cron_timeout) {
		zlog(ZLOG_WARNING,
			"[pool %s] cron.timeout = %ds but global process_control_timeout = %ds; "
			"SIGTERM/SIGQUIT sent to the MASTER (e.g. `docker stop`) kills the current run through "
			"the master's escalation before cron.timeout can act — set process_control_timeout "
			">= %ds in [global] if SIGTERM/docker stop should give this pool time to finish its run",
			wp->config->name, wp->config->cron_timeout, fpm_global_config.process_control_timeout,
			wp->config->cron_timeout);
	}

	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		return -1;
	}
	entry->wp = wp;
	entry->shared = shared;
	entry->next = cron_registry;
	cron_registry = entry;

	return 0;
}
/* }}} */

/* Sleep until `next` (UTC epoch), interruptibly by SIGTERM. Returns 1 if the
 * time was reached, 0 if SIGTERM woke us (then exit cleanly without running the
 * script). One sleep() call per "jump" instead of a per-second loop — monthly
 * schedules would otherwise cause millions of useless wakeups, and sleep() is
 * interrupted by every delivered signal for which we installed a handler (see
 * fpm_pool_cron_sigterm()). */
static int fpm_pool_cron_sleep_until(time_t next) /* {{{ */
{
	for (;;) {
		time_t now, remaining;

		if (cron_term_requested) {
			return 0;
		}

		now = time(NULL);
		if (now >= next) {
			return 1;
		}

		remaining = next - now;
		sleep((unsigned) remaining);
	}
}
/* }}} */

/* cron.log: appends one line per completed run. No rotation, no reopen on
 * reload — this is a plain append-only file, the operator's own job to
 * rotate (same expectation as any other file this project writes to), kept
 * deliberately this simple because the alternative (run count / "overdue"
 * detection in pool.type = status, task 033c's other option) needs shared
 * state and a design of its own that nobody has asked for yet; see
 * docs/cron.md. Format is fixed and grep-able, not configurable: ISO 8601
 * UTC start time, exit code, duration in whole seconds. */
static void fpm_pool_cron_log_run(const char *path, time_t started, time_t ended, int exit_code) /* {{{ */
{
	int fd;
	struct tm tmv;
	char line[160];
	int len;

	gmtime_r(&started, &tmv);
	len = snprintf(line, sizeof(line),
		"%04d-%02d-%02dT%02d:%02d:%02dZ exit=%d duration=%lds\n",
		tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
		tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
		exit_code, (long) (ended - started));
	if (len <= 0 || (size_t) len >= sizeof(line)) {
		return;
	}

	fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
	if (fd < 0) {
		zlog(ZLOG_WARNING, "cron.log: cannot open '%s' (%s)", path, strerror(errno));
		return;
	}

	{
		size_t left = (size_t) len;
		const char *p = line;

		while (left > 0) {
			ssize_t n = write(fd, p, left);

			if (n < 0) {
				if (errno == EINTR) {
					continue;
				}
				zlog(ZLOG_WARNING, "cron.log: write to '%s' failed (%s)", path, strerror(errno));
				break;
			}
			p += n;
			left -= (size_t) n;
		}
	}

	close(fd);
}
/* }}} */

void fpm_pool_cron_child_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_worker_pool_config_s *c = wp->config;
	struct fpm_cron_shared_s *shared = fpm_pool_cron_shared_for(wp);
	struct sigaction sa;
	time_t next, started;
	int exit_code;

	if (!c->cron_parsed_schedule) {
		/* Should not happen — validate() parses the schedule ONCE at startup,
		 * before anything forks. Without it there is no way to calculate the next
		 * due time, so refuse rather than guess. */
		zlog(ZLOG_ERROR, "[pool %s] cron: no parsed schedule, refusing to run", c->name);
		exit(FPM_EXIT_SOFTWARE);
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fpm_pool_cron_sigterm;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);

	fpm_pool_script_install_sapi_overrides();

	next = fpm_cron_schedule_next(c->cron_parsed_schedule, time(NULL), c->cron_timezone);
	if (next == (time_t) -1) {
		/* Last safety net — validate() accepts syntax, not "whether the schedule
		 * can ever occur". See fpm_cron_schedule.h. */
		zlog(ZLOG_ERROR, "[pool %s] cron: schedule '%s' never matches, refusing to run",
			c->name, c->cron_schedule);
		exit(FPM_EXIT_SOFTWARE);
	}

	if (!fpm_pool_cron_sleep_until(next)) {
		/* SIGTERM during sleep — exit cleanly without starting the script.
		 * No child process has been created yet, so there is nothing to orphan. */
		exit(FPM_EXIT_OK);
	}

	/* cron.timeout: watchdog identical to supervisor.stop_timeout (same code,
	 * fpm_pool_watchdog.c), but armed BEFORE starting the script instead of in a
	 * SIGTERM handler. If the script finishes by itself in time, the watchdog
	 * detects it through pidfd (POLLIN when the PROCESS ends, not the script — see
	 * fpm_pool_watchdog.h) and does nothing. Otherwise: SIGKILL. */
	if (c->cron_timeout > 0) {
		fpm_pool_watchdog_arm(getpid(), (unsigned) c->cron_timeout);
	}

	started = time(NULL);
	if (shared) {
		shared->last_run = started;
		shared->running = 1;
	}

	exit_code = fpm_pool_script_run(c->name, c->cron_script);

	if (shared) {
		shared->running = 0;
		shared->last_exit_code = exit_code;
		shared->has_last_exit_code = 1;
		shared->consecutive_failures = (exit_code != 0) ? shared->consecutive_failures + 1 : 0;
	}

	if (c->cron_log && *c->cron_log) {
		fpm_pool_cron_log_run(c->cron_log, started, time(NULL), exit_code);
	}

	/* exit_code != 0 must be visible at warning level, not debug — a single
	 * instance on a VPS has no cluster to catch it. */
	if (exit_code != 0) {
		zlog(ZLOG_WARNING, "[pool %s] cron: script '%s' exited with non-zero code %d",
			c->name, c->cron_script, exit_code);
	}

	/* Always normal exit(0), regardless of the script's exit_code — cron has no
	 * restart/backoff policy like supervisor (see the comment at the top of the
	 * file), so the script exit_code controls nothing beyond this log.
	 * fpm_children.c (pm = static, max_children = 1, UNTOUCHED) respawns this
	 * process unconditionally and immediately; the new process calculates the
	 * next due time from the current clock. */
	exit(FPM_EXIT_OK);
}
/* }}} */

void fpm_pool_cron_status(struct fpm_worker_pool_s *wp, struct fpm_pool_status_s *out) /* {{{ */
{
	struct fpm_cron_shared_s *shared = fpm_pool_cron_shared_for(wp);

	memset(out, 0, sizeof(*out));

	/* next_run is NOT read from shared — calculate it ON DEMAND, exactly as cron
	 * itself calculates the next run (fpm_cron_schedule_next() from "now"). This
	 * works even when shared == NULL (for example, init_main has not run yet) and
	 * is the only reason next_run needs no shared-memory state — see
	 * docs/NOTES.md 3u and 3r. */
	if (wp->config->cron_parsed_schedule) {
		time_t n = fpm_cron_schedule_next(wp->config->cron_parsed_schedule, time(NULL), wp->config->cron_timezone);
		out->has_next_run = 1;
		out->next_run = (n == (time_t) -1) ? 0 : n;
	}

	if (!shared) {
		out->state = FPM_POOL_STATE_IDLE;
		return;
	}

	out->state = shared->running ? FPM_POOL_STATE_RUNNING : FPM_POOL_STATE_IDLE;
	out->last_start = shared->last_run;
	out->last_exit_code = shared->last_exit_code;
	out->has_last_exit_code = shared->has_last_exit_code;
	out->consecutive_failures = shared->consecutive_failures;
}
/* }}} */
