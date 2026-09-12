/* fpm-ng: see fpm_http_direct_ops.h. */

#include "fpm_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <event2/buffer.h>
#include <event2/http.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_type.h"
#include "fpm_scoreboard.h"
#include "fpm_shm.h"
#include "fpm_http_acl.h"
#include "fpm_http_direct_ops.h"
#include "fpm_operator_http.h"
#include "zlog.h"

/* One slot per child. See the header for why there is no lock. */
struct fpm_http_direct_ops_slot {
	unsigned long conn_accepted;
	unsigned long conn_live;		/* gauge, published from the worker's tick */
	unsigned long conn_timed_out;
	unsigned long conn_refused;		/* http.max_connections_per_client, issue #61 */
	unsigned long requests_refused[FPM_HTTP_DIRECT_REFUSED_MAX];
	unsigned long requests_local;
	unsigned long requests_active;
	unsigned long responses_pending;		/* gauge, published from the worker's tick */
	unsigned long responses_rejected;
	unsigned retiring;		/* gauge, issue #65: this child is draining and will exit */
};

/* The version on the page. Bump it when a field changes meaning or leaves;
 * adding one does not need a bump, because a reader that does not know a field
 * ignores it and one that does finds it. This is the contract
 * docs/http-direct.md documents, and the only reason it is a constant here is
 * that the page has to print something a tool can compare against. */
#define FPM_HTTP_DIRECT_SCHEMA 1

struct fpm_http_direct_ops_shared {
	unsigned nslots;
	struct fpm_http_direct_ops_slot slots[];
};

/* wp -> shared counters. A list rather than a field on the pool because
 * fpm_worker_pool_s is upstream's and this is one pool type's business; the
 * same reason fpm_pool_supervisor.c keeps its own registry. Every pool of this
 * type appears exactly once, so the walk is over a handful of entries at child
 * start-up and never again. */
struct fpm_http_direct_ops_entry {
	struct fpm_worker_pool_s *wp;
	struct fpm_http_direct_ops_shared *shared;
	struct fpm_http_direct_ops_entry *next;
};

static struct fpm_http_direct_ops_entry *fpm_http_direct_ops_registry;

struct fpm_http_direct_ops {
	struct fpm_worker_pool_s *wp;
	struct fpm_http_acl_s *acl;		/* NULL = listen.allowed_clients unset */
	const char *ping_path;			/* NULL = ping.path unset */
	const char *ping_response;
	struct fpm_http_direct_ops_shared *shared;
	struct fpm_http_direct_ops_slot *slot;	/* this child's own, NULL if unknown */
	/* What this child has already added to the two connection totals in the
	 * slot. fpm_http_direct_conn.c keeps them as totals since this child
	 * started, so the tick publishes the difference: adding the whole value
	 * every tick would count each drop a hundred times a second, and assigning
	 * it would make the slot's total restart at zero for the next child to
	 * take this index -- which is the one thing a counter must never do. */
	unsigned long published_timed_out;
	unsigned long published_refused;
};

static struct fpm_http_direct_ops_shared *fpm_http_direct_ops_shared_get(struct fpm_worker_pool_s *wp)
{
	struct fpm_http_direct_ops_entry *e;

	for (e = fpm_http_direct_ops_registry; e; e = e->next) {
		if (e->wp == wp) {
			return e->shared;
		}
	}
	return NULL;
}

int fpm_http_direct_ops_init_main(struct fpm_worker_pool_s *wp)
{
	struct fpm_http_direct_ops_entry *entry;
	struct fpm_http_direct_ops_shared *shared;
	unsigned nslots = (unsigned) (wp->config->pm_max_children > 0 ? wp->config->pm_max_children : 1);

	if (fpm_http_direct_ops_shared_get(wp)) {
		/* fpm_conf.c can re-run the per-pool init inside one master process;
		 * a second segment would be one the children never read. This is not
		 * what happens on SIGUSR2: the master re-executes there, so a reload
		 * does start the counters again. Measured on the poligon 2026-09-11 --
		 * accepted conn 9 before, 1 after. Documented in docs/http-direct.md
		 * under "Reset semantics". */
		return 0;
	}
	shared = fpm_shm_alloc(sizeof(*shared) + nslots * sizeof(shared->slots[0]));
	if (!shared) {
		zlog(ZLOG_ERROR, "[pool %s] http-direct: cannot allocate the status counters", wp->config->name);
		return -1;
	}
	shared->nslots = nslots;
	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		zlog(ZLOG_ERROR, "[pool %s] http-direct: cannot register the status counters", wp->config->name);
		return -1;
	}
	entry->wp = wp;
	entry->shared = shared;
	entry->next = fpm_http_direct_ops_registry;
	fpm_http_direct_ops_registry = entry;

	return 0;
}

struct fpm_http_direct_ops *fpm_http_direct_ops_init_child(struct fpm_worker_pool_s *wp)
{
	struct fpm_http_direct_ops *ops = calloc(1, sizeof(*ops));
	struct fpm_scoreboard_s *scoreboard;
	struct fpm_scoreboard_proc_s *proc;

	if (!ops) {
		return NULL;
	}
	ops->wp = wp;
	if (wp->config->listen_allowed_clients && *wp->config->listen_allowed_clients &&
		fpm_http_acl_parse(wp->config->name, "listen.allowed_clients",
			wp->config->listen_allowed_clients, &ops->acl) < 0) {
		/* Logged there, naming the address that did not parse. Refusing to
		 * serve is the same choice fastcgi.c makes: a list that was meant to
		 * keep someone out must never end up keeping nobody out. */
		free(ops);
		return NULL;
	}
	if (wp->config->ping_path && *wp->config->ping_path) {
		ops->ping_path = wp->config->ping_path;
		ops->ping_response = wp->config->ping_response ? wp->config->ping_response : "pong";
	}
	/* pm.status_path is deliberately NOT read here. Issue #275 moved this
	 * pool's status page onto the operator endpoint's listener
	 * (fpm_operator_endpoint.c), which renders it from the same shared counters
	 * through fpm_pool_type_s.operator_status -- so the page is unchanged and
	 * the directive names exactly one page on one socket, which is the rule
	 * #273 was written to restore.
	 *
	 * ping.path above stays, for a different reason: it is a liveness probe for
	 * whatever is in front of the pool, so the public listener is where it
	 * belongs, and #273 kept it there deliberately (point 9). */

	ops->shared = fpm_http_direct_ops_shared_get(wp);
	/* The child's own slot is its scoreboard index: the scoreboard already
	 * hands each child a private slot and keeps it for the child's life, so
	 * reusing that index avoids a second allocator with the same lifetime and
	 * the same failure modes. */
	scoreboard = fpm_scoreboard_get();
	proc = scoreboard ? fpm_scoreboard_proc_get(scoreboard, -1) : NULL;
	if (ops->shared && proc) {
		size_t index = (size_t) (proc - scoreboard->procs);

		if (index < ops->shared->nslots) {
			ops->slot = &ops->shared->slots[index];
			/* Only the gauge is cleared. The three totals belong to the pool,
			 * not to whichever child happens to hold this slot now: a child
			 * recycled by pm.max_requests gets the index the dead one
			 * released, and zeroing here would make the pool's accepted
			 * connections go backwards in the middle of a monitoring series.
			 * The gauges, on the other hand, are exactly what a child killed
			 * mid-request leaks, so they start at zero.
			 *
			 * conn_timed_out and conn_refused are totals and stay: the tick
			 * publishes them as a difference against this child's own
			 * watermark (see published_timed_out), so a successor adds to what
			 * it inherits instead of restarting the series at zero. */
			ops->slot->requests_active = 0;
			ops->slot->conn_live = 0;
			ops->slot->responses_pending = 0;
			/* A replacement is not born retiring, however its predecessor
			 * left. */
			ops->slot->retiring = 0;
		}
	}

	return ops;
}

void fpm_http_direct_ops_free(struct fpm_http_direct_ops *ops)
{
	if (!ops) {
		return;
	}
	fpm_http_acl_free(ops->acl);
	free(ops);
}

void fpm_http_direct_ops_accepted(struct fpm_http_direct_ops *ops)
{
	if (ops && ops->slot) {
		ops->slot->conn_accepted++;
	}
}

void fpm_http_direct_ops_active(struct fpm_http_direct_ops *ops, int delta)
{
	if (!ops || !ops->slot) {
		return;
	}
	if (delta < 0) {
		if (ops->slot->requests_active) {
			ops->slot->requests_active--;
		}
	} else {
		ops->slot->requests_active++;
	}
}

void fpm_http_direct_ops_local(struct fpm_http_direct_ops *ops)
{
	if (ops && ops->slot) {
		ops->slot->requests_local++;
	}
}

void fpm_http_direct_ops_refused(struct fpm_http_direct_ops *ops, enum fpm_http_direct_refusal why)
{
	if (ops && ops->slot && why >= 0 && why < FPM_HTTP_DIRECT_REFUSED_MAX) {
		ops->slot->requests_refused[why]++;
	}
}

void fpm_http_direct_ops_rejected(struct fpm_http_direct_ops *ops)
{
	if (ops && ops->slot) {
		ops->slot->responses_rejected++;
	}
}

void fpm_http_direct_ops_publish(struct fpm_http_direct_ops *ops,
	const struct fpm_http_direct_ops_live *live)
{
	if (!ops || !ops->slot || !live) {
		return;
	}
	ops->slot->conn_live = live->connections;
	ops->slot->responses_pending = live->pending;
	ops->slot->retiring = live->retiring;
	/* The difference since the last publish, not the value: see
	 * published_timed_out. The caller's two numbers only ever grow, so the
	 * subtraction cannot wrap. */
	ops->slot->conn_timed_out += live->timed_out - ops->published_timed_out;
	ops->slot->conn_refused += live->refused_conn - ops->published_refused;
	ops->published_timed_out = live->timed_out;
	ops->published_refused = live->refused_conn;
}

int fpm_http_direct_ops_allowed(struct fpm_http_direct_ops *ops, const char *peer)
{
	if (!ops || !ops->acl) {
		return 1;
	}
	return peer ? fpm_http_acl_check(ops->acl, peer) : 0;
}

static const char *fpm_http_direct_ops_pm_name(int pm)
{
	switch (pm) {
		case PM_STYLE_STATIC: return "static";
		case PM_STYLE_DYNAMIC: return "dynamic";
		case PM_STYLE_ONDEMAND: return "ondemand";
		default: return "unknown";
	}
}

/* Whether the child that owns this slot is still alive, from the scoreboard
 * copy the status page already made. A slot belongs to a scoreboard index and
 * is only ever reset by the next child to take that index, so a child that
 * died -- scaled down, recycled, or killed outright -- leaves its gauges
 * standing until then. Totals are meant to stand; gauges are not, and a "live
 * connections" that only ever ratchets upwards is worse than none.
 *
 * Reading `used` rather than clearing the slot at exit is what also covers the
 * child nothing runs in: SIGKILL, request_terminate_timeout, a crash.
 *
 * The shared scoreboard, not the copy the page took: fpm_scoreboard_copy() is
 * called with copy_procs = 0 here, so the copy has no procs array at all. An
 * unlocked int read is the right price for a gauge that is already up to one
 * tick stale -- taking the scoreboard's lock per slot to decide whether to add
 * a number would cost more than the number is worth. */
static int fpm_http_direct_ops_slot_alive(const struct fpm_scoreboard_s *live, unsigned i)
{
	return live && i < live->nprocs && live->procs[i].used;
}

/* The pool's totals, summed over the slots. Children that have never run leave
 * theirs at zero, so a pool that has not reached pm.max_children still adds up
 * to what it has actually done. The gauges are summed over the live children
 * only, for the reason above. */
static void fpm_http_direct_ops_totals(const struct fpm_http_direct_ops_shared *shared,
	const struct fpm_scoreboard_s *copy, struct fpm_http_direct_ops_slot *out)
{
	unsigned i;

	memset(out, 0, sizeof(*out));
	if (!shared) {
		return;
	}
	for (i = 0; i < shared->nslots; i++) {
		const struct fpm_http_direct_ops_slot *in = &shared->slots[i];
		unsigned r;

		out->conn_accepted += in->conn_accepted;
		out->conn_timed_out += in->conn_timed_out;
		out->conn_refused += in->conn_refused;
		for (r = 0; r < FPM_HTTP_DIRECT_REFUSED_MAX; r++) {
			out->requests_refused[r] += in->requests_refused[r];
		}
		out->requests_local += in->requests_local;
		out->responses_rejected += in->responses_rejected;
		if (fpm_http_direct_ops_slot_alive(copy, i)) {
			out->conn_live += in->conn_live;
			out->requests_active += in->requests_active;
			out->responses_pending += in->responses_pending;
			out->retiring += in->retiring;
		}
	}
}

/* What the "refused requests" row has always meant: requests this pool answered
 * with a refusal. Connections refused by http.max_connections_per_client are
 * not in it -- most of them never became a request -- and have a row of their
 * own. */
static unsigned long fpm_http_direct_ops_refused_total(const struct fpm_http_direct_ops_slot *total)
{
	unsigned long n = 0;
	unsigned r;

	for (r = 0; r < FPM_HTTP_DIRECT_REFUSED_MAX; r++) {
		n += total->requests_refused[r];
	}
	return n;
}

/* The per-child rows, on `?full`. This is what makes the accept distribution
 * measurable from the status page alone, which is the whole point of issue #64
 * -- issue #53's fairness finding needed an external harness to see it, and a
 * pool-wide sum cannot show it at all. A slot that has accepted nothing is
 * printed anyway: "this child got none" is the observation. */
static void fpm_http_direct_ops_status_workers(const struct fpm_http_direct_ops_shared *shared,
	const struct fpm_scoreboard_s *live, int json, int full, struct fpm_operator_buf_s *out)
{
	unsigned i;

	if (!full || !shared) {
		fpm_operator_buf_appendf(out, "%s", json ? "}\n" : "");
		return;
	}
	if (json) {
		fpm_operator_buf_appendf(out, ",\"workers\":[");
	} else {
		fpm_operator_buf_appendf(out, "\n");
	}
	for (i = 0; i < shared->nslots; i++) {
		const struct fpm_http_direct_ops_slot *in = &shared->slots[i];
		int alive = fpm_http_direct_ops_slot_alive(live, i);
		/* The same rule the pool totals use, said out loud per row: the totals
		 * of a child that has gone are still this pool's, its gauges are not.
		 * `live` is on the row so that a reader can tell "this child holds
		 * nothing" from "no child holds this slot". */
		unsigned long conn_live = alive ? in->conn_live : 0;
		unsigned long requests_active = alive ? in->requests_active : 0;
		unsigned long responses_pending = alive ? in->responses_pending : 0;
		unsigned retiring = alive ? in->retiring : 0;
		/* The pid is on the row because it is the address of the retire
		 * signal: issue #65 retires a child with SIGUSR1, and without the pid
		 * here an operator would have to go looking for it in ps and guess
		 * which of the pool's children is the slot they just read. Taken from
		 * the scoreboard rather than kept in the slot -- the scoreboard
		 * already has it, and one copy cannot disagree with itself. */
		int pid = alive ? (int) live->procs[i].pid : 0;

		if (json) {
			fpm_operator_buf_appendf(out,
				"%s{\"slot\":%u,\"live\":%d,\"pid\":%d,\"retiring\":%u,"
				"\"accepted conn\":%lu,\"live connections\":%lu,"
				"\"active requests\":%lu,\"pending responses\":%lu,\"non-php requests\":%lu,"
				"\"refused acl\":%lu,\"refused capacity\":%lu,\"refused connections\":%lu,"
				"\"timed out connections\":%lu,\"rejected responses\":%lu}",
				i ? "," : "", i, alive, pid, retiring, in->conn_accepted, conn_live, requests_active,
				responses_pending, in->requests_local,
				in->requests_refused[FPM_HTTP_DIRECT_REFUSED_ACL],
				in->requests_refused[FPM_HTTP_DIRECT_REFUSED_CAPACITY],
				in->conn_refused, in->conn_timed_out, in->responses_rejected);
		} else {
			fpm_operator_buf_appendf(out,
				"slot:                 %u\n"
				"live:                 %d\n"
				"pid:                  %d\n"
				"retiring:             %u\n"
				"accepted conn:        %lu\n"
				"live connections:     %lu\n"
				"active requests:      %lu\n"
				"pending responses:    %lu\n"
				"non-php requests:     %lu\n"
				"refused acl:          %lu\n"
				"refused capacity:     %lu\n"
				"refused connections:  %lu\n"
				"timed out connections:%lu\n"
				"rejected responses:   %lu\n\n",
				i, alive, pid, retiring, in->conn_accepted, conn_live, requests_active,
				responses_pending, in->requests_local,
				in->requests_refused[FPM_HTTP_DIRECT_REFUSED_ACL],
				in->requests_refused[FPM_HTTP_DIRECT_REFUSED_CAPACITY],
				in->conn_refused, in->conn_timed_out, in->responses_rejected);
		}
	}
	if (json) {
		fpm_operator_buf_appendf(out, "]}\n");
	}
}

/* Rendered from shared memory and configuration only -- see the header of
 * fpm_pool_type_s.operator_status. `wp` is the pool being reported on, which
 * since issue #275 is NOT the pool of the process doing the rendering: the
 * operator endpoint's child answers the request, so the scoreboard comes from
 * wp->scoreboard rather than from fpm_scoreboard_get() and the counters from
 * the registry entry the master filled in before the first fork. Both are
 * shared segments, which is why this works at all, and it is the same foreign
 * read the operator pages do (fpm_operator_pages.c). */
static void fpm_http_direct_ops_status_body(struct fpm_worker_pool_s *wp, int json, int full,
	struct fpm_operator_buf_s *out)
{
	const struct fpm_http_direct_ops_shared *shared = fpm_http_direct_ops_shared_get(wp);
	struct fpm_scoreboard_s *copy = fpm_scoreboard_copy(wp->scoreboard, 0);
	struct fpm_http_direct_ops_slot total;
	char start[64];
	struct tm tm;
	time_t now = time(NULL);

	if (!copy) {
		fpm_operator_buf_appendf(out, "%s", json ? "{}\n" : "unavailable\n");
		return;
	}
	fpm_http_direct_ops_totals(shared, wp->scoreboard, &total);
	if (localtime_r(&copy->start_epoch, &tm) && strftime(start, sizeof(start), "%d/%b/%Y:%H:%M:%S %z", &tm)) {
		/* nothing: start is filled in */
	} else {
		snprintf(start, sizeof(start), "-");
	}

	if (json) {
		fpm_operator_buf_appendf(out,
			"{\"pool\":\"%s\",\"process manager\":\"%s\",\"start time\":\"%s\",\"start since\":%lu,"
			"\"accepted conn\":%lu,\"idle processes\":%d,\"active processes\":%d,\"total processes\":%d,"
			"\"max active processes\":%d,\"max children reached\":%u,\"requests\":%lu,"
			"\"non-php requests\":%lu,\"refused requests\":%lu,\"active requests\":%lu,"
			"\"slow requests\":%lu,\"memory peak\":%zu,"
			"\"direct schema\":%d,\"live connections\":%lu,\"pending responses\":%lu,"
			"\"refused acl\":%lu,\"refused capacity\":%lu,\"refused connections\":%lu,"
			"\"timed out connections\":%lu,\"rejected responses\":%lu,"
			"\"retiring children\":%u",
			copy->pool, fpm_http_direct_ops_pm_name(copy->pm), start,
			(unsigned long) (now - copy->start_epoch), total.conn_accepted,
			copy->idle, copy->active, copy->idle + copy->active, copy->active_max,
			copy->max_children_reached, copy->requests, total.requests_local,
			fpm_http_direct_ops_refused_total(&total), total.requests_active,
			copy->slow_rq, copy->memory_peak,
			FPM_HTTP_DIRECT_SCHEMA, total.conn_live, total.responses_pending,
			total.requests_refused[FPM_HTTP_DIRECT_REFUSED_ACL],
			total.requests_refused[FPM_HTTP_DIRECT_REFUSED_CAPACITY],
			total.conn_refused, total.conn_timed_out, total.responses_rejected, total.retiring);
	} else {
		fpm_operator_buf_appendf(out,
			"pool:                 %s\n"
			"process manager:      %s\n"
			"start time:           %s\n"
			"start since:          %lu\n"
			"accepted conn:        %lu\n"
			"idle processes:       %d\n"
			"active processes:     %d\n"
			"total processes:      %d\n"
			"max active processes: %d\n"
			"max children reached: %u\n"
			"requests:             %lu\n"
			"non-php requests:     %lu\n"
			"refused requests:     %lu\n"
			"active requests:      %lu\n"
			"slow requests:        %lu\n"
			"memory peak:          %zu\n"
			"direct schema:        %d\n"
			"live connections:     %lu\n"
			"pending responses:    %lu\n"
			"refused acl:          %lu\n"
			"refused capacity:     %lu\n"
			"refused connections:  %lu\n"
			"timed out connections:%lu\n"
			"rejected responses:   %lu\n"
			"retiring children:    %u\n",
			copy->pool, fpm_http_direct_ops_pm_name(copy->pm), start,
			(unsigned long) (now - copy->start_epoch), total.conn_accepted,
			copy->idle, copy->active, copy->idle + copy->active, copy->active_max,
			copy->max_children_reached, copy->requests, total.requests_local,
			fpm_http_direct_ops_refused_total(&total), total.requests_active,
			copy->slow_rq, copy->memory_peak,
			FPM_HTTP_DIRECT_SCHEMA, total.conn_live, total.responses_pending,
			total.requests_refused[FPM_HTTP_DIRECT_REFUSED_ACL],
			total.requests_refused[FPM_HTTP_DIRECT_REFUSED_CAPACITY],
			total.conn_refused, total.conn_timed_out, total.responses_rejected, total.retiring);
	}
	fpm_http_direct_ops_status_workers(shared, wp->scoreboard, json, full, out);
	fpm_scoreboard_free_copy(copy);
}

void fpm_http_direct_ops_render_status(struct fpm_worker_pool_s *wp, const char *query,
	struct fpm_operator_reply_s *reply)
{
	int json = fpm_operator_http_has_flag(query, "json");

	reply->content_type = json ? "application/json" : "text/plain; charset=utf-8";
	fpm_http_direct_ops_status_body(wp, json, fpm_operator_http_has_flag(query, "full"), &reply->body);
}

static void fpm_http_direct_ops_send(struct evhttp_request *http, const char *content_type,
	struct evbuffer *body, int *status, size_t *bytes)
{
	struct evkeyvalq *headers = evhttp_request_get_output_headers(http);

	evhttp_add_header(headers, "Content-Type", content_type);
	/* A monitoring page that a proxy is free to cache is a monitoring page
	 * that lies; upstream's fpm_status.c sends the same three. */
	evhttp_add_header(headers, "Expires", "Thu, 01 Jan 1970 00:00:00 GMT");
	evhttp_add_header(headers, "Cache-Control", "no-cache, no-store, must-revalidate, max-age=0");
	*status = 200;
	*bytes = evbuffer_get_length(body);
	evhttp_send_reply(http, 200, "OK", body);
}

/* Matched against the path, with any query string cut off, and matched whole so
 * that "/pings" is not "/ping". No percent-decoding, which is deliberate --
 * ping.path is a literal in the pool file and upstream matches it literally
 * too, so "/%70ing" is not a way past a proxy rule written against the
 * documented spelling.
 *
 * Only ping.path is here. pm.status_path left this listener in issue #275 and
 * is answered by the operator endpoint -- see the note in
 * fpm_http_direct_ops_init_child(). */
int fpm_http_direct_ops_try_local(struct fpm_http_direct_ops *ops, struct evhttp_request *http,
	int *status, size_t *bytes)
{
	const char *uri = evhttp_request_get_uri(http);
	const char *query = uri ? strchr(uri, '?') : NULL;
	size_t path_len;
	struct evbuffer *body;
	char path[512];

	if (!ops || !uri || !ops->ping_path) {
		return 0;
	}
	path_len = query ? (size_t) (query - uri) : strlen(uri);
	if (path_len >= sizeof(path)) {
		return 0;
	}
	memcpy(path, uri, path_len);
	path[path_len] = '\0';
	if (!strcmp(path, ops->ping_path)) {
		body = evbuffer_new();
		if (!body) {
			return 0;
		}
		evbuffer_add_printf(body, "%s", ops->ping_response);
		fpm_http_direct_ops_send(http, "text/plain", body, status, bytes);
		evbuffer_free(body);
		return 1;
	}
	return 0;
}
