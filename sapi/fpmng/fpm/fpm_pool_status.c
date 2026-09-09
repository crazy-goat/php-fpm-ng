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
/* Per-connection recv()/send() limit — see the rationale next to setsockopt()
 * in fpm_pool_status_child_main(). */
#define FPM_POOL_STATUS_IO_TIMEOUT_SEC 5

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

/* Growing buffer — the number of pools is naturally small (the config size),
 * so simplicity (realloc x2) matters more here than avoiding a few allocations. */
struct fpm_status_buf_s {
	char *data;
	size_t len;
	size_t cap;
};

static void fpm_status_buf_free(struct fpm_status_buf_s *b) /* {{{ */
{
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}
/* }}} */

static void fpm_status_buf_appendf(struct fpm_status_buf_s *b, const char *fmt, ...) /* {{{ */
{
	for (;;) {
		size_t avail = b->cap - b->len;
		va_list ap;
		int n;

		va_start(ap, fmt);
		n = vsnprintf(b->data ? b->data + b->len : NULL, avail, fmt, ap);
		va_end(ap);

		if (n < 0) {
			return;
		}
		if ((size_t) n < avail) {
			b->len += (size_t) n;
			return;
		}

		{
			size_t new_cap = b->cap ? b->cap * 2 : 256;
			char *new_data;

			while (new_cap < b->len + (size_t) n + 1) {
				new_cap *= 2;
			}
			new_data = realloc(b->data, new_cap);
			if (!new_data) {
				return;
			}
			b->data = new_data;
			b->cap = new_cap;
		}
	}
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

typedef void (*fpm_pool_status_row_cb)(struct fpm_status_buf_s *b, const struct fpm_pool_status_row_s *row, int first);

static void fpm_pool_status_collect_and_render(struct fpm_status_buf_s *b, fpm_pool_status_row_cb cb) /* {{{ */
{
	struct fpm_worker_pool_s *wp;
	int first = 1;

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		const struct fpm_pool_type_s *type = fpm_pool_type_of(wp);
		struct fpm_pool_status_row_s row;

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

static void fpm_pool_status_row_prometheus(struct fpm_status_buf_s *b, const struct fpm_pool_status_row_s *row, int first) /* {{{ */
{
	static const char *const states[] = { "running", "backoff", "gave_up", "finished", "idle" };
	time_t now = time(NULL);
	size_t i;

	(void) first;

	fpm_status_buf_appendf(b, "fpmng_pool_info{pool=\"%s\",type=\"%s\"} 1\n", row->name, row->type_name);

	if (row->serves_requests) {
		fpm_status_buf_appendf(b, "fpmng_pool_workers_idle{pool=\"%s\"} %d\n", row->name, row->idle);
		fpm_status_buf_appendf(b, "fpmng_pool_workers_active{pool=\"%s\"} %d\n", row->name, row->active);
		fpm_status_buf_appendf(b, "fpmng_pool_requests_total{pool=\"%s\"} %lu\n", row->name, row->requests);
		return;
	}

	for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
		fpm_status_buf_appendf(b, "fpmng_pool_state{pool=\"%s\",state=\"%s\"} %d\n",
			row->name, states[i], row->st.state == (enum fpm_pool_state_e) i);
	}
	fpm_status_buf_appendf(b, "fpmng_pool_last_start_seconds{pool=\"%s\"} %ld\n", row->name, (long) row->st.last_start);
	if (row->st.state == FPM_POOL_STATE_RUNNING && row->st.last_start > 0) {
		fpm_status_buf_appendf(b, "fpmng_pool_uptime_seconds{pool=\"%s\"} %ld\n",
			row->name, (long) (now > row->st.last_start ? now - row->st.last_start : 0));
	}
	if (row->st.has_last_exit_code) {
		fpm_status_buf_appendf(b, "fpmng_pool_last_exit_code{pool=\"%s\"} %d\n", row->name, row->st.last_exit_code);
	}
	fpm_status_buf_appendf(b, "fpmng_pool_consecutive_failures{pool=\"%s\"} %u\n", row->name, row->st.consecutive_failures);
	if (row->st.has_next_run) {
		fpm_status_buf_appendf(b, "fpmng_pool_next_run_seconds{pool=\"%s\"} %ld\n", row->name, (long) row->st.next_run);
	}
	if (row->st.has_backoff_until) {
		fpm_status_buf_appendf(b, "fpmng_pool_backoff_seconds{pool=\"%s\"} %ld\n", row->name,
			(long) (row->st.backoff_until > now ? row->st.backoff_until - now : 0));
	}
}
/* }}} */

static void fpm_pool_status_render_prometheus(struct fpm_status_buf_s *b) /* {{{ */
{
	fpm_status_buf_appendf(b,
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
	fpm_pool_status_collect_and_render(b, fpm_pool_status_row_prometheus);

	/* Application metrics (NOTES 3k): aggregate the time-series tables from ALL
	 * worker slots in shm — read another process's shared memory from this
	 * process, without PHP or request context, so render_text does not touch
	 * ZEND_API. No series (nobody registered anything) = no additional lines,
	 * deliberately without an "occupier" comment. */
	{
		char *text = NULL;
		size_t len = 0;

		if (!fpmng_metrics_render_text(&text, &len) && text && len) {
			fpm_status_buf_appendf(b, "%s", text);
		}
		free(text);
	}
}
/* }}} */

static void fpm_pool_status_row_json(struct fpm_status_buf_s *b, const struct fpm_pool_status_row_s *row, int first) /* {{{ */
{
	time_t now = time(NULL);

	if (!first) {
		fpm_status_buf_appendf(b, ",");
	}

	if (row->serves_requests) {
		fpm_status_buf_appendf(b,
			"{\"name\":\"%s\",\"type\":\"%s\",\"serves_requests\":true,"
			"\"idle\":%d,\"active\":%d,\"requests\":%lu}",
			row->name, row->type_name, row->idle, row->active, row->requests);
		return;
	}

	fpm_status_buf_appendf(b,
		"{\"name\":\"%s\",\"type\":\"%s\",\"serves_requests\":false,"
		"\"state\":\"%s\",\"last_start\":%ld,\"consecutive_failures\":%u",
		row->name, row->type_name, fpm_pool_status_state_name(row->st.state),
		(long) row->st.last_start, row->st.consecutive_failures);
	if (row->st.state == FPM_POOL_STATE_RUNNING && row->st.last_start > 0) {
		fpm_status_buf_appendf(b, ",\"uptime\":%ld",
			(long) (now > row->st.last_start ? now - row->st.last_start : 0));
	}
	if (row->st.has_last_exit_code) {
		fpm_status_buf_appendf(b, ",\"last_exit_code\":%d", row->st.last_exit_code);
	}
	if (row->st.has_next_run) {
		fpm_status_buf_appendf(b, ",\"next_run\":%ld", (long) row->st.next_run);
	}
	if (row->st.has_backoff_until) {
		fpm_status_buf_appendf(b, ",\"backoff_seconds\":%ld",
			(long) (row->st.backoff_until > now ? row->st.backoff_until - now : 0));
	}
	fpm_status_buf_appendf(b, "}");
}
/* }}} */

static void fpm_pool_status_render_json(struct fpm_status_buf_s *b) /* {{{ */
{
	fpm_status_buf_appendf(b, "{\"pools\":[");
	fpm_pool_status_collect_and_render(b, fpm_pool_status_row_json);
	fpm_status_buf_appendf(b, "]}\n");
}
/* }}} */

/* One response, written in full but under a bounded total time. write() is
 * warn_unused_result in glibc and a (void) cast does not satisfy that (task
 * 013's lint job reported both call sites below), so the result is used for
 * something worth doing anyway: the fd is a blocking socket (nothing here or
 * in fpm_sockets.c sets O_NONBLOCK, and accept() does not inherit it on
 * Linux), so a short write means a signal arrived mid-write, and returning
 * there would have truncated a /metrics response. Unlike the access log
 * (fpm_http_access_log.c:146-153), nothing here depends on the payload
 * reaching the fd in a single write() -- one connection carries exactly one
 * response and is then closed.
 *
 * The deadline is why this is not a plain retry loop. SO_SNDTIMEO
 * (FPM_POOL_STATUS_IO_TIMEOUT_SEC, set per connection at :500) is per
 * write() call, not per response, and this pool's pm.max_children is always
 * 1 (validate()), so a client that reads one byte every 4 seconds would make
 * partial progress on every call, restart the 5s timer each time, and pin the
 * only process for as long as it cares to trickle -- exactly the hang the
 * setsockopt() block was added to prevent. A single write() used to bound
 * that implicitly; retrying has to bound it explicitly. Budget is one
 * SO_SNDTIMEO's worth for the whole response, measured on CLOCK_MONOTONIC so
 * a clock step cannot extend it.
 *
 * A peer that hung up, or ran out the budget, is not logged: this endpoint is
 * scraped every 15-60s and a scraper that gives up mid-response would
 * otherwise fill the error log. */
static void fpm_pool_status_write_all(int fd, const char *data, size_t len) /* {{{ */
{
	struct timespec deadline;

	if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
		/* No clock, no bound we can honour -- one write() and be done, which
		 * is the behaviour this function replaced. */
		if (write(fd, data, len) < 0) {
			return;
		}
		return;
	}
	deadline.tv_sec += FPM_POOL_STATUS_IO_TIMEOUT_SEC;

	while (len) {
		struct timespec now;
		ssize_t n = write(fd, data, len);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return;
		}
		if (n == 0) {
			return;
		}
		data += n;
		len -= (size_t) n;

		if (len && clock_gettime(CLOCK_MONOTONIC, &now) == 0
			&& (now.tv_sec > deadline.tv_sec
				|| (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))) {
			return;
		}
	}
}
/* }}} */

/* Raw, minimal HTTP server — deliberately without keep-alive, chunked encoding,
 * or header parsing. This is a monitoring endpoint (Prometheus scrape every
 * 15-60s), not a WWW server; each connection is one request, one response, then
 * close. We care ONLY about the first line ("GET <path> HTTP/1.x"). */
static void fpm_pool_status_handle_conn(int fd) /* {{{ */
{
	char req[4096];
	ssize_t n;
	size_t total = 0;
	char path[256] = "";
	struct fpm_status_buf_s body = {0};
	const char *content_type = "text/plain; charset=utf-8";
	int status_code = 200;
	const char *status_text = "OK";
	char header[256];
	int header_len;

	/* One or more recv() calls, until we see the end of the first line or fill
	 * the buffer — sufficient; "GET /metrics HTTP/1.1\r\n" fits with ample room.
	 * The rest of the request (headers and any body) is ignored — we do not parse
	 * it anyway. */
	while (total < sizeof(req) - 1) {
		n = recv(fd, req + total, sizeof(req) - 1 - total, 0);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		total += (size_t) n;
		req[total] = '\0';
		if (strstr(req, "\r\n") || strstr(req, "\n")) {
			break;
		}
	}

	if (total == 0) {
		return;
	}
	req[total] = '\0';

	{
		char method[16];
		if (sscanf(req, "%15s %255s", method, path) != 2) {
			path[0] = '\0';
		}
	}

	if (!strcmp(path, "/metrics")) {
		fpm_pool_status_render_prometheus(&body);
	} else if (!strcmp(path, "/status")) {
		content_type = "application/json";
		fpm_pool_status_render_json(&body);
	} else {
		status_code = 404;
		status_text = "Not Found";
		fpm_status_buf_appendf(&body, "not found: %s\nknown paths: /metrics /status\n",
			path[0] ? path : "(unparseable request)");
	}

	header_len = snprintf(header, sizeof(header),
		"HTTP/1.1 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n"
		"\r\n",
		status_code, status_text, content_type, body.len);

	if (header_len > 0) {
		fpm_pool_status_write_all(fd, header, (size_t) header_len);
	}
	if (body.data && body.len) {
		fpm_pool_status_write_all(fd, body.data, body.len);
	}

	fpm_status_buf_free(&body);
}
/* }}} */

void fpm_pool_status_child_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	int listen_fd = wp->listening_socket;

	/* Deliberately no custom signal handling: SIGTERM has its default
	 * disposition here (fpm_signals_child_init() sets it for children before
	 * run_child:) — the process simply exits immediately, which is correct
	 * because status has no "current work" to complete (each connection is fully
	 * handled in one accept() cycle and never lasts longer than one recv/send).
	 * On reload (SIGUSR2), fpm_process_ctl.c sends SIGTERM to this pool
	 * directly for exactly that reason — see the comment at
	 * fpm_process_ctl.c:165 and docs/NOTES.md 3x. Outside of reload — an
	 * explicit graceful stop or log rotation — the ordinary SIGQUIT fan-out
	 * still applies, and a delay until the master escalates to SIGTERM there
	 * is known and accepted, the same as for supervisor/cron without a custom
	 * SIGQUIT handler — see docs/NOTES.md 3p, scenario 2. */

	for (;;) {
		int fd = accept(listen_fd, NULL, NULL);
		struct timeval tv;

		if (fd < 0) {
			if (errno == EINTR) {
				continue;
			}
			/* accept() errors on TCP/UDS sockets are usually transient (for
			 * example, ECONNABORTED) — there is no reason to terminate the entire
			 * process because of one failed connection. */
			continue;
		}

		/* A client that opens a connection and never sends anything (or does not
		 * read the response) would hang this pool's ONLY process forever —
		 * pm.max_children is always 1 here (see validate()), so there is no other
		 * worker to take over traffic meanwhile. Timeout both recv() AND send(),
		 * not only accept(). */
		tv.tv_sec = FPM_POOL_STATUS_IO_TIMEOUT_SEC;
		tv.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		fpm_pool_status_handle_conn(fd);
		close(fd);
	}
}
/* }}} */
