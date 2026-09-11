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
#include "fpm_scoreboard.h"
#include "fpm_shm.h"
#include "fpm_http_acl.h"
#include "fpm_http_direct_ops.h"
#include "zlog.h"

/* One slot per child. See the header for why there is no lock. */
struct fpm_http_direct_ops_slot {
	unsigned long conn_accepted;
	unsigned long requests_refused;
	unsigned long requests_local;
	unsigned long requests_active;
};

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
	const char *status_path;		/* NULL = pm.status_path unset */
	struct fpm_http_direct_ops_shared *shared;
	struct fpm_http_direct_ops_slot *slot;	/* this child's own, NULL if unknown */
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
		/* fpm_conf.c re-runs the per-pool init on a reload; the segment from
		 * the previous generation is still mapped and still the one the
		 * children will read. */
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
	if (wp->config->pm_status_path && *wp->config->pm_status_path) {
		ops->status_path = wp->config->pm_status_path;
	}

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
			memset(ops->slot, 0, sizeof(*ops->slot));
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

void fpm_http_direct_ops_refused(struct fpm_http_direct_ops *ops)
{
	if (ops && ops->slot) {
		ops->slot->requests_refused++;
	}
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

/* The pool's totals, summed over the slots. Children that have never run leave
 * theirs at zero, so a pool that has not reached pm.max_children still adds up
 * to what it has actually done. */
static void fpm_http_direct_ops_totals(struct fpm_http_direct_ops *ops, struct fpm_http_direct_ops_slot *out)
{
	unsigned i;

	memset(out, 0, sizeof(*out));
	if (!ops->shared) {
		return;
	}
	for (i = 0; i < ops->shared->nslots; i++) {
		out->conn_accepted += ops->shared->slots[i].conn_accepted;
		out->requests_refused += ops->shared->slots[i].requests_refused;
		out->requests_local += ops->shared->slots[i].requests_local;
		out->requests_active += ops->shared->slots[i].requests_active;
	}
}

static void fpm_http_direct_ops_status_body(struct fpm_http_direct_ops *ops, int json, struct evbuffer *out)
{
	struct fpm_scoreboard_s *copy = fpm_scoreboard_copy(fpm_scoreboard_get(), 0);
	struct fpm_http_direct_ops_slot total;
	char start[64];
	struct tm tm;
	time_t now = time(NULL);

	if (!copy) {
		evbuffer_add_printf(out, "%s", json ? "{}\n" : "unavailable\n");
		return;
	}
	fpm_http_direct_ops_totals(ops, &total);
	if (localtime_r(&copy->start_epoch, &tm) && strftime(start, sizeof(start), "%d/%b/%Y:%H:%M:%S %z", &tm)) {
		/* nothing: start is filled in */
	} else {
		snprintf(start, sizeof(start), "-");
	}

	if (json) {
		evbuffer_add_printf(out,
			"{\"pool\":\"%s\",\"process manager\":\"%s\",\"start time\":\"%s\",\"start since\":%lu,"
			"\"accepted conn\":%lu,\"idle processes\":%d,\"active processes\":%d,\"total processes\":%d,"
			"\"max active processes\":%d,\"max children reached\":%u,\"requests\":%lu,"
			"\"non-php requests\":%lu,\"refused requests\":%lu,\"active requests\":%lu,"
			"\"slow requests\":%lu,\"memory peak\":%zu}\n",
			copy->pool, fpm_http_direct_ops_pm_name(copy->pm), start,
			(unsigned long) (now - copy->start_epoch), total.conn_accepted,
			copy->idle, copy->active, copy->idle + copy->active, copy->active_max,
			copy->max_children_reached, copy->requests, total.requests_local,
			total.requests_refused, total.requests_active, copy->slow_rq, copy->memory_peak);
	} else {
		evbuffer_add_printf(out,
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
			"memory peak:          %zu\n",
			copy->pool, fpm_http_direct_ops_pm_name(copy->pm), start,
			(unsigned long) (now - copy->start_epoch), total.conn_accepted,
			copy->idle, copy->active, copy->idle + copy->active, copy->active_max,
			copy->max_children_reached, copy->requests, total.requests_local,
			total.requests_refused, total.requests_active, copy->slow_rq, copy->memory_peak);
	}
	fpm_scoreboard_free_copy(copy);
}

/* Whether the query string asks for JSON. The same "?json" upstream's status
 * page uses, matched as a bare flag among '&'-separated parameters so that
 * "?json&full" keeps working the day full arrives. */
static int fpm_http_direct_ops_wants_json(struct evhttp_request *http)
{
	const char *uri = evhttp_request_get_uri(http);
	const char *p = uri ? strchr(uri, '?') : NULL;

	while (p) {
		size_t len;

		p++;
		len = strcspn(p, "&");
		if (len == 4 && !strncmp(p, "json", 4)) {
			return 1;
		}
		p = strchr(p, '&');
	}
	return 0;
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

/* Matched against the path, with any query string cut off: "/status?json" is
 * the status page, and a path is compared whole so that "/statuses" is not. No
 * percent-decoding, which is deliberate -- pm.status_path is a literal in the
 * pool file and upstream matches it literally too, so "/%73tatus" is not a way
 * past a proxy rule written against the documented spelling. */
int fpm_http_direct_ops_try_local(struct fpm_http_direct_ops *ops, struct evhttp_request *http,
	int *status, size_t *bytes)
{
	const char *uri = evhttp_request_get_uri(http);
	const char *query = uri ? strchr(uri, '?') : NULL;
	size_t path_len;
	struct evbuffer *body;
	char path[512];

	if (!ops || !uri) {
		return 0;
	}
	if (!ops->ping_path && !ops->status_path) {
		return 0;
	}
	path_len = query ? (size_t) (query - uri) : strlen(uri);
	if (path_len >= sizeof(path)) {
		return 0;
	}
	memcpy(path, uri, path_len);
	path[path_len] = '\0';
	if (ops->ping_path && !strcmp(path, ops->ping_path)) {
		body = evbuffer_new();
		if (!body) {
			return 0;
		}
		evbuffer_add_printf(body, "%s", ops->ping_response);
		fpm_http_direct_ops_send(http, "text/plain", body, status, bytes);
		evbuffer_free(body);
		return 1;
	}
	if (ops->status_path && !strcmp(path, ops->status_path)) {
		int json = fpm_http_direct_ops_wants_json(http);

		body = evbuffer_new();
		if (!body) {
			return 0;
		}
		fpm_http_direct_ops_status_body(ops, json, body);
		fpm_http_direct_ops_send(http, json ? "application/json" : "text/plain", body, status, bytes);
		evbuffer_free(body);
		return 1;
	}

	return 0;
}
