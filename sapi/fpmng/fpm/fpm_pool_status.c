/* fpm-ng: pool.type = status.
 *
 * See fpm_pool_status.h and section 3u of docs/NOTES.md for the design
 * rationale. In short: this is the only pool type that does not start PHP at
 * all — the child runs its own accept loop on its own socket (the same
 * mechanism as listen for fcgi/http, but listening DIRECTLY, without fcgi+1 as
 * the HTTP gateway does) and answers every connection with raw HTTP, without
 * going through PHP or FastCGI.
 *
 * THE DATA SHAPE DIFFERS between pool types (this is the substance of the task,
 * not a detail) — branch on fpm_pool_type_s.serves_requests:
 *   - serves_requests = 1 (fcgi, http): idle/active workers, requests — read
 *     directly from that pool's scoreboard (fpm_scoreboard_copy(), the same
 *     mechanism existing fpm_status.c uses for its OWN pool — here we read
 *     ANOTHER pool's scoreboard, from ANOTHER process, hence a copy instead of
 *     direct reading under the lock).
 *   - serves_requests = 0 (supervisor, cron): state/last_start/exit_code/
 *     consecutive_failures (/next_run for cron) through fpm_pool_type_s.status(),
 *     which each such type implements in its OWN file, reading its OWN shared
 *     memory (fpm_pool_supervisor.c, fpm_pool_cron.c). This file does not know
 *     the internal structure of those states — exactly as required by the
 *     contract in docs/NOTES.md 3h.
 *   - types without .status and with serves_requests = 0 (that is, "status"
 *     itself) — omitted from output entirely, without special treatment by name.
 *
 * The metric label is the pool name — cardinality is naturally bounded (the
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
#include "fpm_pool_status.h"
#include "fpm_operator_http.h"
#include "fpm_pool_type.h"
#include "fpm_scoreboard.h"
#include "php_fpmng_metrics.h"
#include "zlog.h"

/* Rejected, not allowed, directives — see fpm_pool_type_check_directives()
 * in fpm_pool_type.c. Unlike supervisor/cron, "listen"/"listen." are NOT
 * rejected here: status really listens on its own port. The remaining reasons
 * are the same as for supervisor/cron: no FastCGI requests (so request
 * directives, ping., and access. make no sense), no PHP (so pm.* is generated
 * programmatically, always static+1 — there is no "number of processes"
 * directive; one process is entirely sufficient for monitoring scrapes), and
 * directives belonging to the OTHER pool types. */
const char *const fpm_pool_status_rejects[] = {
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
	"cron.",
	"http.",
	"fiber.",
	NULL
};

int fpm_pool_status_validate(struct fpm_worker_pool_s *wp) /* {{{ */
{
	/* One process is entirely sufficient: this is a lightweight, sequential HTTP
	 * server for monitoring scrapes (Prometheus typically every 15-60s), not
	 * public traffic. The absence of a "number of processes" directive is
	 * deliberate — if more were ever needed, that would be a design decision (a
	 * new status.processes directive, modeled on supervisor.processes), not a
	 * default. */
	wp->config->pm = PM_STYLE_STATIC;
	wp->config->pm_max_children = 1;

	return 0;
}
/* }}} */

static const char *fpm_pool_status_state_name(enum fpm_pool_state_e state) /* {{{ */
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

/* Shared by both formats: for every pool in the master, in config order, call
 * the callback with ready data — either from the scoreboard (serves_requests) or
 * from fpm_pool_type_s.status() (the rest). A pool without .status and with
 * serves_requests = 0 (that is, "status" itself) is skipped — there is nothing
 * to show, and no need for special detection by name. */
struct fpm_pool_status_row_s {
	const char *name;
	const char *type_name;
	int serves_requests;

	/* serves_requests = 1 */
	int idle, active;
	unsigned long requests;

	/* serves_requests = 0 */
	struct fpm_pool_status_s st;
};

typedef void (*fpm_pool_status_row_cb)(struct fpm_operator_buf_s *b, const struct fpm_pool_status_row_s *row, int first);

static void fpm_pool_status_collect_and_render(struct fpm_operator_buf_s *b, fpm_pool_status_row_cb cb,
	struct fpm_worker_pool_s *only) /* {{{ */
{
	struct fpm_worker_pool_s *wp;
	int first = 1;

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		const struct fpm_pool_type_s *type = fpm_pool_type_of(wp);
		struct fpm_pool_status_row_s row;

		/* A per-pool operator endpoint (issue #274) reports its OWN pool; the
		 * aggregating pool.type = status passes NULL and reports every pool.
		 * Same walk either way -- the filter is here rather than in a second
		 * collector so that a row added for one of them cannot go missing from
		 * the other. */
		if (only && wp != only) {
			continue;
		}

		memset(&row, 0, sizeof(row));
		row.name = wp->config->name;
		row.type_name = type->name;
		row.serves_requests = type->serves_requests;

		if (type->serves_requests) {
			struct fpm_scoreboard_s *copy = fpm_scoreboard_copy(wp->scoreboard, 0);

			if (!copy) {
				continue;
			}
			row.idle = copy->idle;
			row.active = copy->active;
			row.requests = copy->requests;
			fpm_scoreboard_free_copy(copy);
		} else if (type->status) {
			type->status(wp, &row.st);
		} else {
			/* Type without meaningful state to show (today: "status" itself) —
			 * skip it instead of guessing. */
			continue;
		}

		cb(b, &row, first);
		first = 0;
	}
}
/* }}} */

static void fpm_pool_status_row_prometheus(struct fpm_operator_buf_s *b, const struct fpm_pool_status_row_s *row, int first) /* {{{ */
{
	static const char *const states[] = { "running", "backoff", "gave_up", "finished", "idle" };
	time_t now = time(NULL);
	size_t i;

	(void) first;

	fpm_operator_buf_appendf(b, "fpmng_pool_info{pool=\"%s\",type=\"%s\"} 1\n", row->name, row->type_name);

	if (row->serves_requests) {
		fpm_operator_buf_appendf(b, "fpmng_pool_workers_idle{pool=\"%s\"} %d\n", row->name, row->idle);
		fpm_operator_buf_appendf(b, "fpmng_pool_workers_active{pool=\"%s\"} %d\n", row->name, row->active);
		fpm_operator_buf_appendf(b, "fpmng_pool_requests_total{pool=\"%s\"} %lu\n", row->name, row->requests);
		return;
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
}
/* }}} */

void fpm_pool_status_render_prometheus(struct fpm_operator_buf_s *b, struct fpm_worker_pool_s *only) /* {{{ */
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
		"# TYPE fpmng_pool_backoff_seconds gauge\n");
	fpm_pool_status_collect_and_render(b, fpm_pool_status_row_prometheus, only);

	if (only) {
		/* The application metrics below aggregate every worker slot in shm,
		 * with no pool dimension to filter on, so a per-pool endpoint would be
		 * answering with another pool's numbers. Left out until #276 decides
		 * what a per-pool scrape reports; omitting them is recoverable, showing
		 * the wrong ones is not. */
		return;
	}

	/* Application metrics (NOTES 3k): aggregate the time-series tables from ALL
	 * worker slots in shm — read another process's shared memory from this
	 * process, without PHP or request context, so render_text does not touch
	 * ZEND_API. No series (nobody registered anything) = no additional lines,
	 * deliberately without an "occupier" comment. */
	{
		char *text = NULL;
		size_t len = 0;

		if (!fpmng_metrics_render_text(&text, &len) && text && len) {
			fpm_operator_buf_appendf(b, "%s", text);
		}
		free(text);
	}
}
/* }}} */

static void fpm_pool_status_row_json(struct fpm_operator_buf_s *b, const struct fpm_pool_status_row_s *row, int first) /* {{{ */
{
	time_t now = time(NULL);

	if (!first) {
		fpm_operator_buf_appendf(b, ",");
	}

	if (row->serves_requests) {
		fpm_operator_buf_appendf(b,
			"{\"name\":\"%s\",\"type\":\"%s\",\"serves_requests\":true,"
			"\"idle\":%d,\"active\":%d,\"requests\":%lu}",
			row->name, row->type_name, row->idle, row->active, row->requests);
		return;
	}

	fpm_operator_buf_appendf(b,
		"{\"name\":\"%s\",\"type\":\"%s\",\"serves_requests\":false,"
		"\"state\":\"%s\",\"last_start\":%ld,\"consecutive_failures\":%u",
		row->name, row->type_name, fpm_pool_status_state_name(row->st.state),
		(long) row->st.last_start, row->st.consecutive_failures);
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
	fpm_operator_buf_appendf(b, "}");
}
/* }}} */

void fpm_pool_status_render_json(struct fpm_operator_buf_s *b, struct fpm_worker_pool_s *only) /* {{{ */
{
	fpm_operator_buf_appendf(b, "{\"pools\":[");
	fpm_pool_status_collect_and_render(b, fpm_pool_status_row_json, only);
	fpm_operator_buf_appendf(b, "]}\n");
}
/* }}} */

/* pool.type = status answers two fixed paths and aggregates every pool, which
 * is what it was before issue #274 and what it stays. The per-pool endpoint
 * built on the same server has a configurable path per format and one pool to
 * report on -- see fpm_operator_endpoint.c. */
static void fpm_pool_status_dispatch(void *ctx, const char *path, struct fpm_operator_reply_s *reply) /* {{{ */
{
	(void) ctx;

	if (!strcmp(path, "/metrics")) {
		fpm_pool_status_render_prometheus(&reply->body, NULL);
		reply->handled = 1;
	} else if (!strcmp(path, "/status")) {
		reply->content_type = "application/json";
		fpm_pool_status_render_json(&reply->body, NULL);
		reply->handled = 1;
	}
}
/* }}} */

void fpm_pool_status_child_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	fpm_operator_http_serve(wp->listening_socket, fpm_pool_status_dispatch, NULL, "/metrics /status");
}
/* }}} */
