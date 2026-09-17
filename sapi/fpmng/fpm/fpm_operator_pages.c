/* fpm-ng: the two operator pages -- Prometheus text and JSON -- for one pool.
 *
 * This was pool.type = status, a pool that ran no PHP, listened on its own
 * socket and aggregated every pool in the master. Issue #278 removed that pool
 * and kept these renderers: the same two formats, now produced for the pool
 * that configured the path and served by fpm_operator_endpoint.c on the
 * operator listener. See docs/operator-endpoint.md.
 *
 * THE DATA SHAPE DIFFERS between pool types (this is the substance, not a
 * detail) -- branch on fpm_pool_type_s.serves_requests:
 *   - serves_requests = 1 (fcgi, http): idle/active workers and the type's
 *     baseline counter, read from that pool's scoreboard with
 *     fpm_scoreboard_copy() -- a copy, because the renderer runs in the
 *     operator endpoint's process and not in one of the pool's own children.
 *   - serves_requests = 0 (supervisor, cron): state/last_start/exit_code/
 *     consecutive_failures (/next_run for cron) through fpm_pool_type_s.status(),
 *     which each such type implements in its OWN file, reading its OWN shared
 *     memory (fpm_pool_supervisor.c, fpm_pool_cron.c). This file does not know
 *     the internal structure of those states -- exactly as required by the
 *     contract in docs/NOTES.md 3h.
 *
 * The metric label is the pool name -- cardinality is naturally bounded (the
 * number of pools in the config). No labels with unbounded cardinality (no
 * request path, no timestamp as a label, etc.).
 */

#include "fpm_config.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_operator_pages.h"
#include "fpm_operator_http.h"
#include "fpm_pool_type.h"
#include "fpm_scoreboard.h"
#include "fpm_metrics.h"
#include "php_fpmng_metrics.h"
#include "zlog.h"

static const char *fpm_operator_page_state_name(enum fpm_pool_state_e state) /* {{{ */
{
	switch (state) {
		case FPM_POOL_STATE_RUNNING:  return "running";
		case FPM_POOL_STATE_BACKOFF:  return "backoff";
		case FPM_POOL_STATE_GAVE_UP:  return "gave_up";
		case FPM_POOL_STATE_FINISHED: return "finished";
		case FPM_POOL_STATE_IDLE:     return "idle";
	}
	return "unknown";
}
/* }}} */

/* Shared by both formats: the one pool this endpoint reports on, with ready
 * data — either from the scoreboard (serves_requests) or from
 * fpm_pool_type_s.status() (the rest). Before issue #278 this walked every pool
 * in the master, in config order, because one page covered all of them; a page
 * is now a directive on the pool it describes, so the walk is gone and the row
 * is the pool's own. */
struct fpm_operator_page_row_s {
	const char *name;
	const char *type_name;
	int serves_requests;

	/* The type's baseline counter (issue #277): its short name, from
	 * fpm_pool_type_s.baseline_counter, and the value, from wherever that type
	 * keeps it. Reported on every pool whether or not its script ever called
	 * fpm_metric_*(). NULL name = this type counts nothing. */
	const char *counter;
	unsigned long counter_value;

	/* serves_requests = 1 */
	int idle, active;

	/* serves_requests = 0 */
	struct fpm_pool_status_s st;

	/* fpm_pool_type_s.live_gauges() (issue #333): extra per-pool gauges no
	 * other field above has room for, additive on top of whichever shape
	 * serves_requests picked. live_count may be 0 even when the type sets
	 * live_gauges (nothing to report right now); it is NEVER consulted when
	 * live_count is 0, so a type that leaves live_gauges NULL needs no
	 * special-casing here -- the memset() below already leaves live_count 0. */
	struct fpm_pool_live_gauge_s live[FPM_POOL_LIVE_GAUGES_MAX];
	int live_count;
};

typedef void (*fpm_operator_page_row_cb)(struct fpm_operator_buf_s *b, const struct fpm_operator_page_row_s *row);

/* Gather one pool's row and hand it to the format that asked for it. Both
 * formats go through here so that a field added for one of them cannot go
 * missing from the other -- which is the whole reason this is a callback and
 * not two copies of the branch on serves_requests. */
static void fpm_operator_page_collect(struct fpm_operator_buf_s *b, fpm_operator_page_row_cb cb,
	struct fpm_worker_pool_s *wp) /* {{{ */
{
	const struct fpm_pool_type_s *type = fpm_pool_type_of(wp);
	struct fpm_operator_page_row_s row;

	memset(&row, 0, sizeof(row));
	row.name = wp->config->name;
	row.type_name = type->name;
	row.serves_requests = type->serves_requests;
	row.counter = type->baseline_counter;

	if (type->serves_requests) {
		struct fpm_scoreboard_s *copy = fpm_scoreboard_copy(wp->scoreboard, 0);

		if (!copy) {
			return;
		}
		row.idle = copy->idle;
		row.active = copy->active;
		/* The scoreboard already counts what this type calls an invocation,
		 * and has since upstream: the baseline counter for a request-serving
		 * pool is that number under a name, not a second count of the same
		 * thing. */
		row.counter_value = copy->requests;
		fpm_scoreboard_free_copy(copy);
	} else if (type->status) {
		type->status(wp, &row.st);
		row.counter_value = row.st.baseline;
	} else {
		/* A type with no worker counts and no state to report. Nothing has
		 * this shape since issue #278 removed pool.type = status, which did --
		 * it reported on others and not on itself -- but a row is skipped
		 * rather than guessed at, because the alternative is a page of zeroes
		 * that looks like a measurement. */
		return;
	}

	/* Issue #333: orthogonal to the branch above -- a type may set
	 * live_gauges whether or not it serves_requests, since it reports
	 * something neither idle/active/baseline_counter nor fpm_pool_status_s
	 * has a field for. Clamped defensively; every implementation today fills
	 * exactly what it declares, but a future one returning too many is a
	 * truncated page, not a buffer overrun. */
	if (type->live_gauges) {
		row.live_count = type->live_gauges(wp, row.live);
		if (row.live_count < 0) {
			row.live_count = 0;
		} else if (row.live_count > FPM_POOL_LIVE_GAUGES_MAX) {
			row.live_count = FPM_POOL_LIVE_GAUGES_MAX;
		}
	}

	cb(b, &row);
}
/* }}} */

/* Issue #333: HELP/TYPE inline with the value rather than pre-declared in
 * fpm_operator_page_render_prometheus() below, unlike every other series
 * there. Those are fixed for every build; live_gauges' names are per-type
 * (today: worker only) and this file must not learn which type that is, so
 * it cannot pre-declare a name it does not know at compile time. Prometheus's
 * text format allows HELP/TYPE anywhere before the first sample of a series,
 * and a single-pool page never repeats a series, so this is still valid
 * output. */
static void fpm_operator_page_row_prometheus_live(struct fpm_operator_buf_s *b, const struct fpm_operator_page_row_s *row) /* {{{ */
{
	int i;

	for (i = 0; i < row->live_count; i++) {
		fpm_operator_buf_appendf(b,
			"# HELP fpmng_pool_%s %s\n"
			"# TYPE fpmng_pool_%s gauge\n"
			"fpmng_pool_%s{pool=\"%s\"} %ld\n",
			row->live[i].json_key, row->live[i].help,
			row->live[i].json_key,
			row->live[i].json_key, row->name, row->live[i].value);
	}
}
/* }}} */

static void fpm_operator_page_row_prometheus(struct fpm_operator_buf_s *b, const struct fpm_operator_page_row_s *row) /* {{{ */
{
	static const char *const states[] = { "running", "backoff", "gave_up", "finished", "idle" };
	time_t now = time(NULL);
	size_t i;

	fpm_operator_buf_appendf(b, "fpmng_pool_info{pool=\"%s\",type=\"%s\"} 1\n", row->name, row->type_name);

	/* The baseline counter (issue #277) is emitted for every type that has one,
	 * under the name the type chose, and it is the line that used to be spelled
	 * out here as fpmng_pool_requests_total for request-serving pools -- same
	 * name, same value, now data on the type instead of a literal. */
	if (row->serves_requests) {
		fpm_operator_buf_appendf(b, "fpmng_pool_workers_idle{pool=\"%s\"} %d\n", row->name, row->idle);
		fpm_operator_buf_appendf(b, "fpmng_pool_workers_active{pool=\"%s\"} %d\n", row->name, row->active);
		if (row->counter) {
			fpm_operator_buf_appendf(b, "fpmng_pool_%s_total{pool=\"%s\"} %lu\n",
				row->counter, row->name, row->counter_value);
		}
		fpm_operator_page_row_prometheus_live(b, row);
		return;
	}

	if (row->counter) {
		fpm_operator_buf_appendf(b, "fpmng_pool_%s_total{pool=\"%s\"} %lu\n",
			row->counter, row->name, row->counter_value);
	}

	for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
		fpm_operator_buf_appendf(b, "fpmng_pool_state{pool=\"%s\",state=\"%s\"} %d\n",
			row->name, states[i], row->st.state == (enum fpm_pool_state_e) i);
	}
	fpm_operator_buf_appendf(b, "fpmng_pool_last_start_seconds{pool=\"%s\"} %ld\n", row->name, (long) row->st.last_start);
	if (row->st.state == FPM_POOL_STATE_RUNNING && row->st.last_start > 0) {
		fpm_operator_buf_appendf(b, "fpmng_pool_uptime_seconds{pool=\"%s\"} %ld\n",
			row->name, (long) (now > row->st.last_start ? now - row->st.last_start : 0));
	}
	if (row->st.has_last_exit_code) {
		fpm_operator_buf_appendf(b, "fpmng_pool_last_exit_code{pool=\"%s\"} %d\n", row->name, row->st.last_exit_code);
	}
	fpm_operator_buf_appendf(b, "fpmng_pool_consecutive_failures{pool=\"%s\"} %u\n", row->name, row->st.consecutive_failures);
	if (row->st.has_next_run) {
		fpm_operator_buf_appendf(b, "fpmng_pool_next_run_seconds{pool=\"%s\"} %ld\n", row->name, (long) row->st.next_run);
	}
	if (row->st.has_backoff_until) {
		fpm_operator_buf_appendf(b, "fpmng_pool_backoff_seconds{pool=\"%s\"} %ld\n", row->name,
			(long) (row->st.backoff_until > now ? row->st.backoff_until - now : 0));
	}
	/* cron.expect_within (issue #327): 0/1 gauge rather than omitting the
	 * series when not stale, for the same reason fpmng_pool_state above is one
	 * line per possible state instead of only the active one -- a scraper can
	 * alert on "this series is absent" only if it is never absent while the
	 * directive is configured. Omitted entirely (not "always 0") when
	 * cron.expect_within is unset, exactly like has_next_run/has_backoff_until:
	 * a scrape target that never asked for staleness detection reports
	 * nothing about it, not a manufactured zero.
	 *
	 * has_expect_within is set as soon as the directive is configured (see
	 * fpm_pool_cron_status()), not gated on a run having happened yet, which is
	 * what makes the "never absent while configured" claim above actually
	 * true: a freshly started pool with cron.expect_within set reports
	 * fpmng_pool_stale = 0 immediately, rather than omitting the series until
	 * its first run. */
	if (row->st.has_expect_within) {
		fpm_operator_buf_appendf(b, "fpmng_pool_stale{pool=\"%s\"} %d\n", row->name, row->st.stale ? 1 : 0);
		if (row->st.stale) {
			fpm_operator_buf_appendf(b, "fpmng_pool_stale_since_seconds{pool=\"%s\"} %ld\n",
				row->name, (long) row->st.stale_since);
		}
	}
	/* fpmng_supervisor_heartbeat() (issue #327): age in seconds since the last
	 * call, the same derived-at-render-time shape as uptime/backoff_seconds
	 * above. Absent until the script has called it at least once. */
	if (row->st.has_heartbeat) {
		fpm_operator_buf_appendf(b, "fpmng_pool_heartbeat_age_seconds{pool=\"%s\"} %ld\n", row->name,
			(long) (now > row->st.last_heartbeat ? now - row->st.last_heartbeat : 0));
	}
	fpm_operator_page_row_prometheus_live(b, row);
}
/* }}} */

void fpm_operator_page_render_prometheus(struct fpm_operator_buf_s *b, struct fpm_worker_pool_s *wp) /* {{{ */
{
	fpm_operator_buf_appendf(b,
		"# HELP fpmng_pool_info Pool identity and type.\n"
		"# TYPE fpmng_pool_info gauge\n"
		"# HELP fpmng_pool_workers_idle Idle worker processes (pools that serve requests).\n"
		"# TYPE fpmng_pool_workers_idle gauge\n"
		"# HELP fpmng_pool_workers_active Active worker processes (pools that serve requests).\n"
		"# TYPE fpmng_pool_workers_active gauge\n"
		"# HELP fpmng_pool_requests_total Requests served since start (pools that serve requests).\n"
		"# TYPE fpmng_pool_requests_total counter\n"
		"# HELP fpmng_pool_runs_total Scheduled runs started since start, cron only.\n"
		"# TYPE fpmng_pool_runs_total counter\n"
		"# HELP fpmng_pool_restarts_total Times the supervised script was started again, supervisor only.\n"
		"# TYPE fpmng_pool_restarts_total counter\n"
		"# HELP fpmng_pool_state Current pool state; exactly one state label is 1.\n"
		"# TYPE fpmng_pool_state gauge\n"
		"# HELP fpmng_pool_last_start_seconds Unix time of the last start, 0 = never.\n"
		"# TYPE fpmng_pool_last_start_seconds gauge\n"
		"# HELP fpmng_pool_uptime_seconds Elapsed time of the currently running script.\n"
		"# TYPE fpmng_pool_uptime_seconds gauge\n"
		"# HELP fpmng_pool_last_exit_code Exit code of the last finished run.\n"
		"# TYPE fpmng_pool_last_exit_code gauge\n"
		"# HELP fpmng_pool_consecutive_failures Consecutive failed runs.\n"
		"# TYPE fpmng_pool_consecutive_failures gauge\n"
		"# HELP fpmng_pool_next_run_seconds Unix time of the next scheduled run, cron only.\n"
		"# TYPE fpmng_pool_next_run_seconds gauge\n"
		"# HELP fpmng_pool_backoff_seconds Seconds remaining in supervisor backoff.\n"
		"# TYPE fpmng_pool_backoff_seconds gauge\n"
		"# HELP fpmng_pool_stale 1 if a scheduled run is overdue past cron.expect_within, cron only.\n"
		"# TYPE fpmng_pool_stale gauge\n"
		"# HELP fpmng_pool_stale_since_seconds Unix time the overdue run was due; present only while fpmng_pool_stale is 1.\n"
		"# TYPE fpmng_pool_stale_since_seconds gauge\n"
		"# HELP fpmng_pool_heartbeat_age_seconds Seconds since fpmng_supervisor_heartbeat() was last called, supervisor only.\n"
		"# TYPE fpmng_pool_heartbeat_age_seconds gauge\n");
	fpm_operator_page_collect(b, fpm_operator_page_row_prometheus, wp);

	/* Application metrics (NOTES 3k): the time-series tables written by workers
	 * into shm — read another process's shared memory from this process,
	 * without PHP or request context, so neither renderer touches ZEND_API. No
	 * series (nobody registered anything) = no additional lines, deliberately
	 * without an "occupier" comment.
	 *
	 * An endpoint renders its own pool's worker slots only (#276): a pool owns
	 * a contiguous run of them, so the filter is a slot range rather than a
	 * string match on the pool= label, and a pool that registered nothing
	 * renders nothing instead of the other pools' numbers.
	 *
	 * The pool= label stays even though on a per-pool endpoint it is constant:
	 * it is what lets a scraper that reads several pools' endpoints put the
	 * series side by side, and dropping it would make the same series name mean
	 * a different pool depending on which port it came from. */
	{
		char *text = NULL;
		size_t len = 0;

		if (!fpm_metrics_render_pool(wp, &text, &len) && text && len) {
			fpm_operator_buf_appendf(b, "%s", text);
		}
		free(text);
	}
}
/* }}} */

/* Issue #333: same live_gauges array the Prometheus renderer's
 * fpm_operator_page_row_prometheus_live() reads, emitted before the object's
 * closing brace rather than as a nested object -- flat keys, same shape
 * row->counter's single extra key already uses above. */
static void fpm_operator_page_row_json_live(struct fpm_operator_buf_s *b, const struct fpm_operator_page_row_s *row) /* {{{ */
{
	int i;

	for (i = 0; i < row->live_count; i++) {
		fpm_operator_buf_appendf(b, ",\"%s\":%ld", row->live[i].json_key, row->live[i].value);
	}
}
/* }}} */

static void fpm_operator_page_row_json(struct fpm_operator_buf_s *b, const struct fpm_operator_page_row_s *row) /* {{{ */
{
	time_t now = time(NULL);

	/* The baseline counter's JSON key is the short name the type chose, which is
	 * why "requests" below is not written out: on a request-serving pool that IS
	 * the type's counter, and the object it produces is the one this page has
	 * always produced. */
	if (row->serves_requests) {
		fpm_operator_buf_appendf(b,
			"{\"name\":\"%s\",\"type\":\"%s\",\"serves_requests\":true,"
			"\"idle\":%d,\"active\":%d",
			row->name, row->type_name, row->idle, row->active);
		if (row->counter) {
			fpm_operator_buf_appendf(b, ",\"%s\":%lu", row->counter, row->counter_value);
		}
		fpm_operator_page_row_json_live(b, row);
		fpm_operator_buf_appendf(b, "}");
		return;
	}

	fpm_operator_buf_appendf(b,
		"{\"name\":\"%s\",\"type\":\"%s\",\"serves_requests\":false,"
		"\"state\":\"%s\",\"last_start\":%ld,\"consecutive_failures\":%u",
		row->name, row->type_name, fpm_operator_page_state_name(row->st.state),
		(long) row->st.last_start, row->st.consecutive_failures);
	if (row->counter) {
		fpm_operator_buf_appendf(b, ",\"%s\":%lu", row->counter, row->counter_value);
	}
	if (row->st.state == FPM_POOL_STATE_RUNNING && row->st.last_start > 0) {
		fpm_operator_buf_appendf(b, ",\"uptime\":%ld",
			(long) (now > row->st.last_start ? now - row->st.last_start : 0));
	}
	if (row->st.has_last_exit_code) {
		fpm_operator_buf_appendf(b, ",\"last_exit_code\":%d", row->st.last_exit_code);
	}
	if (row->st.has_next_run) {
		fpm_operator_buf_appendf(b, ",\"next_run\":%ld", (long) row->st.next_run);
	}
	if (row->st.has_backoff_until) {
		fpm_operator_buf_appendf(b, ",\"backoff_seconds\":%ld",
			(long) (row->st.backoff_until > now ? row->st.backoff_until - now : 0));
	}
	if (row->st.has_expect_within) {
		fpm_operator_buf_appendf(b, ",\"stale\":%s", row->st.stale ? "true" : "false");
		if (row->st.stale) {
			fpm_operator_buf_appendf(b, ",\"stale_since\":%ld", (long) row->st.stale_since);
		}
	}
	if (row->st.has_heartbeat) {
		fpm_operator_buf_appendf(b, ",\"heartbeat_age\":%ld",
			(long) (now > row->st.last_heartbeat ? now - row->st.last_heartbeat : 0));
	}
	fpm_operator_page_row_json_live(b, row);
	fpm_operator_buf_appendf(b, "}");
}
/* }}} */

void fpm_operator_page_render_json(struct fpm_operator_buf_s *b, struct fpm_worker_pool_s *wp) /* {{{ */
{
	/* An array of one. The shape is what the aggregate page produced before
	 * issue #278, and keeping it means a client that already parses
	 * {"pools":[...]} keeps working against the per-pool address without
	 * learning a second shape. */
	fpm_operator_buf_appendf(b, "{\"pools\":[");
	fpm_operator_page_collect(b, fpm_operator_page_row_json, wp);
	fpm_operator_buf_appendf(b, "]}\n");
}
/* }}} */
