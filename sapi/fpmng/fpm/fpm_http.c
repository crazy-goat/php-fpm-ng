/* Plain HTTP gateway (--with-fpm-http, needs libevent).
 *
 * Under pool.type = gateway (issue #388) the master forks http.gateways
 * processes that serve HTTP/HTTPS on the pool's `listen` address with
 * libevent's evhttp, sharing one listening socket. A gateway behaves like a
 * web server in front of OTHER pools: http.route[] selects a target pool per
 * path prefix, and for each target it talks that target's protocol (FastCGI,
 * or HTTP/1.1 for an http-direct pool via fpm_http_client.c) over a small set
 * of persistent connections, turning the response back into HTTP. Neither the
 * FastCGI code nor the PHP workers know that HTTP exists. Keep-alive, chunked
 * request bodies, HEAD and request parsing are evhttp's job. Without libevent
 * the gateway is compiled out and FPM behaves as before.
 *
 * Before issue #388 this file also ran the pool.type = http weld: the same
 * gateway processes served a sibling pool of PHP workers that shared the
 * section, and its `listen` was the FastCGI socket while the public port was
 * http.listen (the FastCGI port + 1 by default), with the pool itself as the
 * implicit target 0. The gateway type is the proxy half alone: no PHP, no
 * pm.max_children, no implicit target, and `listen` is the public port. The
 * route machinery, the budget below and the transports are unchanged.
 *
 * Persistent connections: a kept connection pins one target worker, so all the
 * gateways of one pool together never hold more than the target's
 * pm.max_children of them. A request beyond that is rejected with 503 +
 * Retry-After, not queued: it sits on target->waiting only for the one dispatch
 * round that fails to take a budget slot, and that round then drains the whole
 * queue to 503 (fpm_http_pump_once() says why -- the wait would be unbounded
 * and invisible to the client). The budget is per target and a counter in
 * shared memory rather than a fixed share per process, so a gateway that
 * happens to get all the clients can still use every worker. A worker waiting
 * for the next request on a kept connection counts as active, so dynamic
 * spawns spare workers for everyone else as it should. ondemand never reaps
 * such a worker though, and with any pm a pinned worker is unavailable to
 * other FastCGI clients (nginx, the status page), so an idle connection is
 * dropped after FPM_HTTP_IDLE_MS (env override, 0 keeps them forever).
 */

#include "fpm_config.h"

#include "fpm.h"
#include "fpm_http.h"

#ifdef HAVE_FPM_HTTP

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <grp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/types.h>
/* musl does not ship <sys/queue.h>, and libevent's headers may pull in a partial
 * one, so include it when it exists and fill in only what is missing. */
#if defined(__has_include)
# if __has_include(<sys/queue.h>)
#  include <sys/queue.h>
# endif
#endif

#ifndef TAILQ_HEAD
#define TAILQ_HEAD(name, type)						\
struct name {								\
	struct type *tqh_first;						\
	struct type **tqh_last;						\
}
#endif
#ifndef TAILQ_ENTRY
#define TAILQ_ENTRY(type)						\
struct {								\
	struct type *tqe_next;						\
	struct type **tqe_prev;						\
}
#endif
#ifndef TAILQ_INIT
#define TAILQ_INIT(head) do {						\
	(head)->tqh_first = NULL;					\
	(head)->tqh_last = &(head)->tqh_first;				\
} while (0)
#endif
#ifndef TAILQ_EMPTY
#define TAILQ_EMPTY(head)	((head)->tqh_first == NULL)
#endif
#ifndef TAILQ_FIRST
#define TAILQ_FIRST(head)	((head)->tqh_first)
#endif
#ifndef TAILQ_NEXT
#define TAILQ_NEXT(elm, field)	((elm)->field.tqe_next)
#endif
#ifndef TAILQ_FOREACH
#define TAILQ_FOREACH(var, head, field)					\
	for ((var) = TAILQ_FIRST(head); (var); (var) = TAILQ_NEXT(var, field))
#endif
#ifndef TAILQ_INSERT_TAIL
#define TAILQ_INSERT_TAIL(head, elm, field) do {			\
	(elm)->field.tqe_next = NULL;					\
	(elm)->field.tqe_prev = (head)->tqh_last;			\
	*(head)->tqh_last = (elm);					\
	(head)->tqh_last = &(elm)->field.tqe_next;			\
} while (0)
#endif
#ifndef TAILQ_REMOVE
#define TAILQ_REMOVE(head, elm, field) do {				\
	if (((elm)->field.tqe_next) != NULL)				\
		(elm)->field.tqe_next->field.tqe_prev =			\
		    (elm)->field.tqe_prev;				\
	else								\
		(head)->tqh_last = (elm)->field.tqe_prev;		\
	*(elm)->field.tqe_prev = (elm)->field.tqe_next;			\
} while (0)
#endif
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>

#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/keyvalq_struct.h>
#include <event2/util.h>

#include "php.h"
#include "fastcgi.h"
#include "zend_smart_str.h"

#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_sockets.h"
#include "fpm_cleanup.h"
#include "fpm_signals.h"
#include "fpm_env.h"
#include "fpm_shm.h"
#include "fpm_atomic.h"
#include "fpm_process_ctl.h"
#include "fpm_http_acl.h"
#include "fpm_http_accept_backoff.h"
#include "fpm_http_forwarded.h"
#include "fpm_acme_challenge.h"
#include "fpm_http_auth.h"
#include "fpm_http_access_log.h"
/* For FPM_HTTP_HEADER_NAME_MAX and FPM_HTTP_HEADERS_MAX: the bound on a
 * request header name (issue #115) and on the whole request header block
 * (issue #117) is the same on both transports, so each has one definition. */
#include "fpm_http_direct_request.h"
#include "fpm_children_extra.h"
/* issue #340: http.route[] asks a target pool's type what it speaks, through
 * fpm_pool_type_resolve() and the serves_fastcgi/serves_http11 bits. */
#include "fpm_pool_type.h"
#include "fpm_tls_http.h"
#include "fpm_tls_reload.h"
#include "fpm_http_static.h"
#include "fpm_child_error_log.h"
#include "fpm_error_log_follow.h"
/* issue #341: fpm_operator_buf_s/fpm_operator_buf_appendf, for
 * fpm_http_render_metrics_prometheus() in fpm_http_route.c. */
#include "fpm_operator_http.h"
/* issue #389: fpm_operator_endpoint_route(), the config-time route table the
 * gateway's http.operator forwarding map is built from. */
#include "fpm_operator_endpoint.h"
#include "zlog.h"

#define FPM_HTTP_IDLE_MS         500		/* http.idle_timeout default (ms); release a pinned worker after this much idle time */
#define FPM_HTTP_KEEPALIVE_TIMEOUT_MS 60000	/* http.keepalive_timeout default (ms); how long an idle keep-alive client connection may wait for its next request */
#define FPM_HTTP_RESPONSE_BUFFER (1024 * 1024)	/* http.response_buffer default (bytes), issue #596 */
#define FPM_HTTP_WRITE_TIMEOUT_MS 30000		/* http.write_timeout default (ms); how long a client may make no progress on a pending response write */
#define FPM_HTTP_READ_TIMEOUT_MS 5000		/* http.read_timeout default (ms); one budget for reading the whole request (headers + body) */
#define FPM_HTTP_MAX_BODY        (32 * 1024 * 1024)	/* http.max_body default; the gateway buffers a whole request body in memory (task 031) */
/* Largest content length we put in a FastCGI record. The protocol allows
 * 0xffff, but every record we emit is padded to an 8-byte boundary
 * (fpm_http_fcgi_record()) and php-src rejects a PARAMS record whose
 * contentLength + paddingLength exceeds 0xffff
 * (main/fastcgi.c, `if (len + padding > FCGI_MAX_LENGTH) return 0;` in
 * fcgi_read_request()) -- the worker then closes the connection with no reply
 * and the gateway answers 502. Measured on 192.168.8.50, 2026-09-09: a request
 * header block of 64560 bytes produced a 65533-byte PARAMS record, padding 3,
 * and a 502. nginx never hit this because it emits padding 0. The largest
 * multiple of 8 below 0xffff needs no padding at all and leaves every shorter
 * record's padding inside the limit. Issue #117. */
#define FCGI_MAX_RECORD_LEN      65528

typedef struct _fpm_http_conn fpm_http_conn;
typedef struct _fpm_http_upstream fpm_http_upstream;

/* Everything the two transports share -- the transport vtable, the target,
 * route and gateway structs, the connection and upstream structs and the
 * helpers both files call -- lives in fpm_http_internal.h. Issue #344 moved it
 * there when the HTTP/1.1 client transport arrived; the definitions moved
 * verbatim, comments included. */
#include "fpm_http_internal.h"

struct fpm_http_gateway_s *gateways = NULL;


void fpm_http_pump(struct fpm_http_gateway_s *gw);
/* Issue #390: referenced by fpm_http_client_track() before its definition. */
static void fpm_http_client_closed(struct evhttp_connection *evcon, void *arg);
/* Issue #390 review: the budget helpers below also maintain this process's own
 * upstreams_held gauge, defined with the other counter helpers further down. */
static void fpm_http_counter_decr(atomic_t *counter);
static atomic_t *fpm_http_target_held(struct fpm_http_target_s *t);

/* Claims one of the TARGET pool's workers for a persistent connection, or
 * fails when they are all taken. Per target since issue #340: two prefixes
 * routed to one pool share this counter, two prefixes routed to two pools do
 * not, because the workers enforcing the limit are one set per pool. */
int fpm_http_budget_take(struct fpm_http_target_s *t)
{
	while (1) {
		unsigned long used = *t->upstreams_used;	/* atomic_t is an integer of some width on every branch of fpm_atomic.h */

		if (used >= t->max_upstreams) {
			return 0;
		}
		if (atomic_cmp_set(t->upstreams_used, used, used + 1)) {
			/* Issue #390 review: the shared reservation first, then this
			 * process's own held gauge. The order is what lets the master
			 * reconcile a process killed in the window between the two: with
			 * the shared half already charged it fails closed (a leaked
			 * reservation, never a stolen one). */
			fpm_http_counter_incr(fpm_http_target_held(t));
			return 1;
		}
	}
}

void fpm_http_budget_give_back(struct fpm_http_target_s *t)
{
	/* The mirror of take's order, and for the same reason: drop this process's
	 * gauge first, so a death in the window leaves the shared budget charged
	 * (fail closed) rather than charged twice when the master reconciles. */
	fpm_http_counter_decr(fpm_http_target_held(t));
	while (1) {
		unsigned long used = *t->upstreams_used;	/* atomic_t is an integer of some width on every branch of fpm_atomic.h */

		if (used == 0 || atomic_cmp_set(t->upstreams_used, used, used - 1)) {
			return;
		}
	}
}

/* Issue #341: the same cmp-set loop as the budget above, for the two
 * monotonic counters a target's metrics report. No saturating case to worry
 * about here (unlike budget_give_back's `used == 0` guard) -- a counter only
 * ever goes up. `counter` may be NULL in principle -- fpm_http_target_init()
 * refuses to start the gateway if the shm allocation failed, so this is a
 * defensive check, not a reachable path, kept only because every other
 * dereference of one of these fields in this file makes the same check. */
void fpm_http_counter_incr(atomic_t *counter)
{
	unsigned long value;

	if (!counter) {
		return;
	}
	do {
		value = *counter;
	} while (!atomic_cmp_set(counter, value, value + 1));
}

/* Issue #390: the decrementing half, for the one gauge (connections_open). A
 * gauge, unlike the counters above, can go down; guarded at zero so a close
 * that races a missing increment cannot underflow the unsigned atomic. No
 * locking, same cmp-set loop as its siblings. */
static void fpm_http_counter_decr(atomic_t *counter)
{
	unsigned long value;

	if (!counter) {
		return;
	}
	do {
		value = *counter;
		if (value == 0) {
			return;
		}
	} while (!atomic_cmp_set(counter, value, value - 1));
}

/* Issue #390 review: return a whole reservation to the shared budget when the
 * master discovers that the process that made it is gone. `amount` is that
 * process's final upstreams_held count; the value can exceed the budget only
 * if the process died inside fpm_http_budget_take()'s one-instruction window
 * between the shared increment and its own gauge's (take does the shared half
 * first, precisely so any such window fails closed), so the subtraction is
 * clamped rather than allowed to wrap. Same cmp-set loop as its siblings. */
static void fpm_http_counter_sub(atomic_t *counter, unsigned long amount)
{
	unsigned long value, dec;

	if (!counter || amount == 0) {
		return;
	}
	do {
		value = *counter;
		dec = amount < value ? amount : value;
		if (dec == 0) {
			return;
		}
	} while (!atomic_cmp_set(counter, value, value - dec));
}

/* Issue #390: the counters segment's two variable-length regions. The segment
 * is ONE flat atomic_t array because C lets a struct have only one flexible
 * array; these two accessors are the only place the layout arithmetic lives.
 * FPM_HTTP_COUNTERS_SLOT_CELLS is the slot stride (the three cells of
 * fpm_http_counters_slot), and the process blocks follow the slots, each
 * (1 + nslots) cells: [connections_open, upstreams_held[0 .. nslots)]. */
#define FPM_HTTP_COUNTERS_SLOT_CELLS 3u

/* First cell of target row i: [requests_total, rejected_total, shared budget]. */
atomic_t *fpm_http_counters_slot_cells(struct fpm_http_counters_s *c, unsigned i)
{
	return &c->cells[(size_t) i * FPM_HTTP_COUNTERS_SLOT_CELLS];
}

/* First cell of gateway process p's own gauge block. */
atomic_t *fpm_http_counters_gauges(struct fpm_http_counters_s *c, unsigned p)
{
	return &c->cells[(size_t) c->nslots * FPM_HTTP_COUNTERS_SLOT_CELLS
		+ (size_t) p * (1u + c->nslots)];
}

size_t fpm_http_counters_size_of(unsigned nslots, unsigned nproc)
{
	return sizeof(struct fpm_http_counters_s)
		+ ((size_t) nslots * FPM_HTTP_COUNTERS_SLOT_CELLS
			+ (size_t) nproc * (1u + nslots)) * sizeof(atomic_t);
}

size_t fpm_http_counters_size(const struct fpm_http_counters_s *c)
{
	return fpm_http_counters_size_of(c->nslots, c->nproc);
}


/* Issue #390: the row a request this gateway answered itself belongs to --
 * ping, static files, the ACME challenge, a 404 for a path no route covers, and
 * the ACL/operator-namespace 403s. Null-safe so a gateway whose segment
 * allocation failed (and which fpm_http_target_init() refuses to start) cannot
 * turn a request into a crash. */
void fpm_http_count_local(struct fpm_http_gateway_s *gw)
{
	if (gw && gw->counters) {
		fpm_http_counter_incr(&fpm_http_counters_slot_cells(gw->counters, gw->counters->nslots - 1)[0]);
	}
}

/* Issue #390: ping.path is also its own counter, so "is the probe answered at
 * all" is readable separately from "how much local traffic there is". */
static void fpm_http_count_ping(struct fpm_http_gateway_s *gw)
{
	fpm_http_count_local(gw);
	if (gw && gw->counters) {
		fpm_http_counter_incr(&gw->counters->ping_total);
	}
}

/* Issue #390 review: this process's own upstreams_held cell for target t, or
 * NULL before the process has claimed its gauge block (a request cannot reach
 * here before fpm_http_gateway_run() sets gw->gauges, so this is defensive).
 * Only this process ever writes its cells -- the master zeroes them after it is
 * gone -- so no shared atomic is involved. */
static atomic_t *fpm_http_target_held(struct fpm_http_target_s *t)
{
	if (!t->gw || !t->gw->gauges) {
		return NULL;
	}
	return &t->gw->gauges[1 + t->slot_index];
}

/* Issue #390: the pool-wide connections_open the renderer reports: the sum of
 * every gateway process's own cell. A process that died with connections open
 * has had its block zeroed by the master, so its connections left the sum with
 * it -- the whole reason the gauge is per-process (see fpm_http_counters_s). */
unsigned long fpm_http_connections_open(struct fpm_http_gateway_s *gw)
{
	unsigned p;
	unsigned long open = 0;

	if (!gw || !gw->counters) {
		return 0;
	}
	for (p = 0; p < gw->counters->nproc; p++) {
		open += (unsigned long) fpm_http_counters_gauges(gw->counters, p)[0];
	}
	return open;
}

/* Issue #390 review: a gateway process is gone (SIGKILL, OOM, crash), and the
 * OS has closed its sockets -- but its per-process cells still count the
 * connections it held, because a killed process runs no close callback. The
 * master calls this from fpm_http_gateway_on_exit(), before it respawns the
 * slot, to make the renderer's sums drop with the process and to give its
 * upstream reservations back to the shared budget:
 *
 *   - connections_open: zeroing the cell is enough, the renderer sums cells.
 *   - upstreams_held[row]: return each to that row's shared upstreams_budget,
 *     so a crash does not shrink the pool's admission budget for the life of
 *     the segment; then zero the cell so the rendered gauge follows.
 *
 * The subtraction is clamped (fpm_http_counter_sub()) because a process killed
 * inside budget_take()'s shared-then-own window can have charged the shared
 * budget without its own gauge; the ordering there makes that leak by at most
 * one, never an under-count of another process's reservation. */
void fpm_http_counters_process_gone(struct fpm_http_gateway_s *gw, unsigned index)
{
	atomic_t *gauges;
	unsigned i;

	if (!gw || !gw->counters || index >= gw->counters->nproc) {
		return;
	}
	gauges = fpm_http_counters_gauges(gw->counters, index);
	gauges[0] = 0;
	for (i = 0; i < gw->counters->nslots; i++) {
		unsigned long held = (unsigned long) gauges[1 + i];
		atomic_t *budget = &fpm_http_counters_slot_cells(gw->counters, i)[2];

		if (held) {
			fpm_http_counter_sub(budget, held);
			gauges[1 + i] = 0;
		}
	}
}

/* Local (server-side) address and port of one HTTP connection, for SERVER_ADDR/SERVER_PORT.
 * Unlike the pool's listen address (which may be a wildcard "*"), this is the real address
 * the client actually connected to -- correct even under SO_REUSEPORT or 0.0.0.0 binds.
 * Both out buffers are left empty ("") when nothing sensible can be reported (e.g. the
 * gateway's own HTTP listener is a unix socket, or the fd is not available). */
void fpm_http_local_addr(struct evhttp_connection *evcon, char *addr_buf, size_t addr_size,
		char *port_buf, size_t port_size)
{
	struct bufferevent *bev;
	evutil_socket_t fd = -1;
	struct sockaddr_storage ss;
	socklen_t sslen = sizeof(ss);

	addr_buf[0] = '\0';
	port_buf[0] = '\0';
	if (!evcon) {
		return;
	}
	bev = evhttp_connection_get_bufferevent(evcon);
	if (bev) {
		fd = bufferevent_getfd(bev);
	}
	if (fd < 0) {
		return;
	}
	if (getsockname(fd, (struct sockaddr*)&ss, &sslen) != 0) {
		return;
	}
	if (ss.ss_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in*)&ss;

		evutil_inet_ntop(AF_INET, &sin->sin_addr, addr_buf, addr_size);
		snprintf(port_buf, port_size, "%u", (unsigned) ntohs(sin->sin_port));
	} else if (ss.ss_family == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6*)&ss;

		evutil_inet_ntop(AF_INET6, &sin6->sin6_addr, addr_buf, addr_size);
		snprintf(port_buf, port_size, "%u", (unsigned) ntohs(sin6->sin6_port));
	}
	/* AF_UNIX: no numeric SERVER_ADDR/SERVER_PORT to report, buffers stay empty */
}

const char *fpm_http_method_name(enum evhttp_cmd_type type);

/* access.suppress_path[]: matched the same way ping.path is (see
 * fpm_http_serve_ping()) -- whole path, query string cut off, no
 * percent-decoding, so the entry an operator writes in the pool file is what
 * is compared against. This is the http gateway's first consumer of the
 * directive (issue #382); it was parsed in fpm_conf.c and copied onto
 * gw->suppress_paths in fpm_http_gateway_settings() but never referenced
 * here before. */
static int fpm_http_log_suppressed(struct fpm_http_gateway_s *gw, struct evhttp_request *req)
{
	char path[512];
	unsigned i;

	if (!gw->suppress_paths_count || !fpm_http_raw_path(req, path, sizeof(path))) {
		return 0;
	}
	for (i = 0; i < gw->suppress_paths_count; i++) {
		if (!strcmp(path, gw->suppress_paths[i])) {
			return 1;
		}
	}
	return 0;
}

/* Single choke point for the access log: pulls method/URI/protocol/Referer/User-Agent
 * straight from the evhttp_request, callers only supply what they already know
 * (effective remote_addr, remote_user if any, final status, body bytes sent).
 *
 * A locally answered ping.path (issue #382) IS logged here, like every other
 * locally answered response (an ACME challenge, a static file) already was --
 * it is real HTTP traffic that reached this process, and an operator who
 * wants it out of the log has the same lever as for any other noisy path:
 * access.suppress_path[], applied by fpm_http_log_suppressed() above.
 *
 * target (issue #341): the backend pool this request was dispatched to, NULL
 * for a request this gateway answered on its own (ping, static, ACME, an ACL
 * rejection before routing ran) -- see fpm_http_access_log.h for the exact
 * field this produces. Gated on gw->has_routes here, not left to the caller:
 * a gateway with no http.route[] configured logs "-" on every line whether or
 * not the caller happens to know its one implicit target's name, which is
 * what keeps that gateway's log byte-for-byte what it was before this issue. */
void fpm_http_log_response(struct fpm_http_gateway_s *gw, struct evhttp_request *req,
		const char *remote_addr, const char *remote_user, int status, size_t bytes, const char *target)
{
	if (!gw->access_log) {
		return;
	}
	if (fpm_http_log_suppressed(gw, req)) {
		return;
	}
	fpm_http_access_log_write(gw->access_log, remote_addr, remote_user,
		fpm_http_method_name(evhttp_request_get_command(req)), evhttp_request_get_uri(req),
		req->major, req->minor, status, bytes,
		evhttp_find_header(evhttp_request_get_input_headers(req), "Referer"),
		evhttp_find_header(evhttp_request_get_input_headers(req), "User-Agent"),
		/* Issue #389: an operator-forwarded request passes the literal
		 * "operator" here even though it was dispatched to a target, so the
		 * target field is non-NULL and the field prints. The has_routes gate
		 * keeps a gateway that never opted into routing (and therefore has no
		 * target field to print) byte-for-byte what it was before #341. */
		(gw->has_routes || target) ? target : NULL);
}

/* ---------------------------------------------------------------- upstream connections */

/* Unlinks the upstream and releases everything it holds except the struct
 * itself. Separate from the free() below because the free can be deferred (see
 * fpm_http_upstream_drop()) while none of this may be: the shared connection
 * slot is the load-bearing one. fpm_http_pump() runs inside that deferred
 * window, and a slot still held by a connection that is already gone makes
 * fpm_http_budget_take() fail, which sends the whole gw->waiting queue a 503
 * "the pool is full" for a pool that has just freed a slot. */
static void fpm_http_upstream_detach(fpm_http_upstream *up)
{
	struct fpm_http_target_s *t = up->t;

	TAILQ_REMOVE(&t->upstreams, up, link);
	t->nupstreams--;
	/* Deregistered here, freed with the struct. When the free is deferred that
	 * incidentally keeps event_free() out of the callback of the event being
	 * freed -- but only then: every other drop still frees from the callback,
	 * which is the open question of issue #132. */
	if (up->ev_read) {
		event_del(up->ev_read);
	}
	if (up->ev_write) {
		event_del(up->ev_write);
	}
	close(up->fd);
	up->fd = -1;
	smart_str_free(&up->pending);
	fpm_http_budget_give_back(t);
}

void fpm_http_upstream_free(fpm_http_upstream *up)
{
	if (up->ev_read) {
		event_free(up->ev_read);
	}
	if (up->ev_write) {
		event_free(up->ev_write);
	}
	free(up);
}

void fpm_http_upstream_drop(fpm_http_upstream *up)
{
	fpm_http_upstream_detach(up);
	if (up->active) {
		/* A caller up the stack still reads this struct, so the free() waits
		 * for it: it checks `dead` and calls fpm_http_upstream_free() on its
		 * way out. The upstream is already off gw->upstreams, so
		 * fpm_http_pump() cannot hand a request to a connection that is
		 * gone. Issue #129. */
		up->dead = 1;
		return;
	}
	fpm_http_upstream_free(up);
}

/* the pool went away mid-request or while idle */
void fpm_http_upstream_fail(fpm_http_upstream *up, int clean_eof)
{
	struct fpm_http_gateway_s *gw = up->gw;
	/* The address named in the two lines below is the TARGET's, not the
	 * gateway pool's own: with http.route[] they are different pools, and a
	 * line that named the gateway would send the operator to the wrong one.
	 * For a gateway with no routes the two are the same string. */
	const char *upstream_address = up->t->listen_address;
	/* Saved before anything else runs: both branches below report this errno,
	 * and event_base_gettimeofday_cached() may fall through to a real
	 * gettimeofday(), which is free to overwrite it. */
	int err = errno;
	/* The gateway wrote a complete request and got nothing at all back. Issue
	 * #117 arrived in the log as "Connection reset by peer" + "no answer",
	 * which is also what an OOM-killed child produces, and root-causing it
	 * needed instrumentation in main/fastcgi.c because the log named no
	 * suspect. It is the same event on the wire either way, so the line below
	 * names both causes and the one place where they differ: a child that died
	 * is reported by the master, a child that refused the head keeps running
	 * and the master stays quiet.
	 *
	 * "Written" means the bytes left this process, not that the worker read
	 * them: a request small enough to fit the socket buffer is fully written
	 * even towards a worker that never read one byte. That is why the line
	 * below still names the child death first and does not claim the worker
	 * parsed anything. */
	int mute = up->busy && up->req_written && !up->reply_seen;

	if (mute) {
		struct timeval now;
		double ms;

		/* the cached loop time: no syscall, and the close is a later loop
		 * iteration than the write, so the two values do differ */
		event_base_gettimeofday_cached(gw->base, &now);
		ms = (now.tv_sec - up->req_written_at.tv_sec) * 1000.0
			+ (now.tv_usec - up->req_written_at.tv_usec) / 1000.0;
		zlog(ZLOG_WARNING, "[pool %s] http: upstream '%s' closed %.1f ms after the complete request was "
			"written to it, without one byte of a reply (%s): the worker died, or it refused the request "
			"head and closed without answering -- if the master reports no child exit for this pool, the "
			"request head is the remaining suspect",
			gw->pool, upstream_address, ms, clean_eof ? "EOF" : strerror(err));
	} else if (!clean_eof) {
		zlog(ZLOG_WARNING, "[pool %s] http: upstream '%s': %s", gw->pool, upstream_address, strerror(err));
	}
	/* EOF is normal after pm.max_requests or a worker restart; a request in flight is lost though */
	if (up->current) {
		fpm_http_conn *c = up->current;

		if (c->headers_sent && !c->discard_upstream) {
			/* A close-delimited HTTP reply ends by this very EOF, but its
			 * transport completes it before it gets here, so reaching this
			 * point with headers out is always a lost reply (issue #533). */
			zlog(ZLOG_WARNING, "[pool %s] http: upstream '%s' failed after the response head was sent; "
				"the client connection is closed without completing the reply",
				gw->pool, upstream_address);
			fpm_http_finish_truncated(c);
		} else {
			fpm_http_finish(c, clean_eof || mute);
		}
		up->current = NULL;
	}
	up->t->ops->drop(up);
	fpm_http_pump(gw);
}

/* one request finished on this connection, it is free for the next */
void fpm_http_request_done(fpm_http_upstream *up)
{
	if (up->current) {
		fpm_http_finish(up->current, 1);
		up->current = NULL;
	}
	up->busy = 0;
	up->req_written = up->reply_seen = 0;
	memset(up->rec_hdr, 0, sizeof(up->rec_hdr));
	up->rec_hdr_len = up->rec_type = up->rec_len = up->rec_pad = 0;
	if (up->gw->idle_ms > 0) {
		event_add(up->ev_read, &up->gw->idle_timeout);
	}
	fpm_http_pump(up->gw);
}

/* Feeds bytes from the pool into the record parser. */
static void fpm_http_upstream_data(fpm_http_upstream *up, const char *buf, size_t len)
{
	if (len > 0) {
		up->reply_seen = 1;
	}
	/* The loop below dereferences `up` after every fpm_http_request_done(),
	 * which can reach fpm_http_upstream_drop() on this same upstream through
	 * fpm_http_pump(). Announce the caller so the drop is deferred, and check
	 * for it after each such call. Issue #129. */
	up->active++;
	while (len > 0) {
		size_t take;

		if (up->rec_len == 0 && up->rec_pad == 0) {
			take = MIN(sizeof(up->rec_hdr) - up->rec_hdr_len, len);
			memcpy(up->rec_hdr + up->rec_hdr_len, buf, take);
			up->rec_hdr_len += take;
			buf += take;
			len -= take;
			if (up->rec_hdr_len < (int)sizeof(up->rec_hdr)) {
				break;
			}
			up->rec_hdr_len = 0;
			up->rec_type = up->rec_hdr[1];
			up->rec_len = (up->rec_hdr[4] << 8) | up->rec_hdr[5];
			up->rec_pad = up->rec_hdr[6];
			if (up->rec_len == 0 && up->rec_pad == 0 && up->rec_type == FCGI_END_REQUEST) {
				fpm_http_request_done(up);
				if (up->dead) {
					break;
				}
			}
			continue;
		}
		if (up->rec_len > 0) {
			take = MIN((size_t)up->rec_len, len);
			if (up->rec_type == FCGI_STDOUT && up->current) {
				fpm_http_stdout(up->current, buf, take);
			} else if (up->rec_type == FCGI_STDERR) {
				zlog(ZLOG_NOTICE, "[pool %s] http: %.*s", up->gw->pool, (int)take, buf);
			}
			up->rec_len -= take;
		} else {
			take = MIN((size_t)up->rec_pad, len);
			up->rec_pad -= take;
		}
		buf += take;
		len -= take;
		if (up->rec_len == 0 && up->rec_pad == 0 && up->rec_type == FCGI_END_REQUEST) {
			fpm_http_request_done(up);
			if (up->dead) {
				break;
			}
		}
	}
	up->active--;
	if (up->dead && !up->active) {
		/* Only the free() was deferred; the connection slot went back to the
		 * pool at detach time and whoever dropped this upstream has already
		 * pumped, so there is nothing to dispatch from here. Issue #129. */
		fpm_http_upstream_free(up);
	}
}

static void fpm_http_upstream_readcb(evutil_socket_t fd, short what, void *arg)
{
	fpm_http_upstream *up = arg;
	char buf[16 * 1024];
	ssize_t n;

	if (what & EV_TIMEOUT) {
		/* idle for a while: give the worker back to the pool */
		if (!up->busy) {
			fpm_http_upstream_drop(up);
		}
		return;
	}
	do {
		n = read(fd, buf, sizeof(buf));
	} while (n < 0 && errno == EINTR);

	if (n > 0) {
		fpm_http_upstream_data(up, buf, n);
	} else if (n == 0 || !fpm_http_would_block(errno)) {
		fpm_http_upstream_fail(up, n == 0);
	}
}

/* Test-only fault injection: makes the Nth write() towards the pool fail with
 * ECONNRESET, in this gateway process, without touching the socket.
 *
 * It exists because the bug of issue #129 lives in a window nothing outside
 * the process can time: a *synchronous* hard write error on the first write of
 * a request, which frees the connection and the upstream underneath their
 * caller. From the outside one can arrange a dead worker, but not that the
 * error surfaces on write() rather than one loop iteration later on read().
 *
 * It is a pool directive (http.fault_upstream_write) and deliberately NOT an
 * environment variable, unlike the other knobs of this file: a knob that
 * fabricates request failures must not be reachable by exporting a name into
 * the master's environment, where an operator would see a 502 and a
 * "Connection reset by peer" line with nothing in the configuration to explain
 * them. The two env knobs it would otherwise resemble are no precedent --
 * FPM_HTTP_MAX_UPSTREAMS is read only for a capacity_override pool and
 * FPMNG_FLOCK_POLL_ATTEMPTS lives in fiber code, so neither is in a stock
 * binary's default path. Unset costs one comparison against 0 per write. */
static int fpm_http_upstream_write_must_fail(struct fpm_http_gateway_s *gw)
{
	if (gw->fault_write_at <= 0) {
		return 0;
	}
	return ++gw->fault_writes == gw->fault_write_at;
}

/* Writes whatever is pending; registers the write event only when the socket is full. */
void fpm_http_upstream_flush(fpm_http_upstream *up)
{
	while (up->pending.s && up->pending_off < ZSTR_LEN(up->pending.s)) {
		ssize_t n;

		if (fpm_http_upstream_write_must_fail(up->gw)) {
			n = -1;
			errno = ECONNRESET;
		} else {
			n = write(up->fd, ZSTR_VAL(up->pending.s) + up->pending_off, ZSTR_LEN(up->pending.s) - up->pending_off);
		}

		if (n > 0) {
			up->pending_off += n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else if (n < 0 && fpm_http_would_block(errno)) {
			event_add(up->ev_write, NULL);
			return;
		} else {
			fpm_http_upstream_fail(up, 0);
			return;
		}
	}
	smart_str_free(&up->pending);
	up->pending_off = 0;
	if (up->busy && !up->req_written) {
		up->req_written = 1;
		event_base_gettimeofday_cached(up->gw->base, &up->req_written_at);
	}
}

static void fpm_http_upstream_writecb(evutil_socket_t fd, short what, void *arg)
{
	fpm_http_upstream *up = arg;

	if (up->connecting) {
		int err = 0;
		socklen_t len = sizeof(err);

		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
			errno = err;
			fpm_http_upstream_fail(up, 0);
			return;
		}
		up->connecting = 0;
		event_add(up->ev_read, NULL);
	}
	fpm_http_upstream_flush(up);
}

void fpm_http_upstream_write(fpm_http_upstream *up, const char *data, size_t len)
{
	smart_str_appendl(&up->pending, data, len);
	if (!up->connecting) {
		fpm_http_upstream_flush(up);
	}
}

/* The FastCGI transport's connect(): one persistent FastCGI connection to a
 * target pool. Reached only through fpm_http_target_fastcgi_ops below -- issue
 * #340 moved it behind that pointer so #344 can add an HTTP/1.1 one next to it
 * without a second dispatch path. */
/* Which connect failures mean "this target cannot be reached" -- as opposed to
 * a busy one (EAGAIN: the unix backlog is full) or this process running out of
 * descriptors, both of which stay a pool-full condition exactly as before. */
static int fpm_http_connect_errno_unreachable(int err)
{
	switch (err) {
	case ENOENT:
	case ECONNREFUSED:
	case EACCES:
	case EPERM:
	case ENOTSOCK:
	case EADDRNOTAVAIL:
	case ENETUNREACH:
	case EHOSTUNREACH:
		return 1;
	default:
		return 0;
	}
}

fpm_http_upstream *fpm_http_transport_connect(struct fpm_http_target_s *t)
{
	struct fpm_http_gateway_s *gw = t->gw;
	fpm_http_upstream *up;

	if (!fpm_http_budget_take(t)) {
		return NULL;
	}
	up = calloc(1, sizeof(*up));
	up->gw = gw;
	up->t = t;
	up->fd = socket(t->upstream_addr.ss_family, SOCK_STREAM, 0);
	if (up->fd < 0) {
		t->connect_errno = fpm_http_connect_errno_unreachable(errno) ? errno : 0;
		free(up);
		fpm_http_budget_give_back(t);
		return NULL;
	}
	evutil_make_socket_nonblocking(up->fd);
	if (t->upstream_addr.ss_family != AF_UNIX) {
		int on = 1;

		setsockopt(up->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	}
	up->ev_read = event_new(gw->base, up->fd, EV_READ | EV_PERSIST, t->ops->on_readable, up);
	up->ev_write = event_new(gw->base, up->fd, EV_WRITE, fpm_http_upstream_writecb, up);

	if (connect(up->fd, (struct sockaddr*)&t->upstream_addr, t->upstream_len) != 0) {
		if (errno != EINPROGRESS) {
			t->connect_errno = fpm_http_connect_errno_unreachable(errno) ? errno : 0;
			event_free(up->ev_read);
			event_free(up->ev_write);
			close(up->fd);
			free(up);
			fpm_http_budget_give_back(t);
			return NULL;
		}
		up->connecting = 1;
		event_add(up->ev_write, NULL);
	} else {
		event_add(up->ev_read, NULL);
	}
	TAILQ_INSERT_TAIL(&t->upstreams, up, link);
	t->nupstreams++;
	return up;
}

/* The FastCGI transport. Bound to a target at config time by
 * fpm_http_target_bind_transport(); nothing outside these four functions and
 * fpm_http_build_request() (which they call) names a FCGI_* symbol. */
const struct fpm_http_transport_s fpm_http_target_fastcgi_ops = {
	fpm_http_transport_connect,
	fpm_http_build_request,
	fpm_http_upstream_readcb,
	fpm_http_upstream_drop
};

/* One dispatch round. Never called directly -- fpm_http_pump() below owns the
 * re-entrancy rules. */
/* Answers one queued request 503 + Retry-After and frees it. Lifted out of
 * the drain loop below without changing a byte of what it sends, so that
 * loop and the wait policy's two rejection sites (a full queue, an expired
 * wait) share one implementation. `c` must already be unlinked from
 * gw->waiting -- see the drain loop's own comment for why that order and not
 * the other one. */
static void fpm_http_reject_queued(fpm_http_conn *c)
{
	c->status = FPM_HTTP_SERVICE_UNAVAIL;
	/* Issue #341: this is the ONE place every rejection funnels through --
	 * a full queue drained here, the queue-cap check at enqueue time, and
	 * fpm_http_wait_expired() below all call this function rather than
	 * reject a request themselves -- so it is also the one place that has to
	 * name the target for both the WARNING and rejected_total. */
	zlog(ZLOG_WARNING, "[pool %s] http: pool full, rejecting a queued request target=%s",
		c->gw->pool, c->target->pool);
	fpm_http_counter_incr(c->target->rejected_total);
	if (c->evcon) {
		struct evbuffer *body = evbuffer_new();

		evhttp_add_header(evhttp_request_get_output_headers(c->req), "Retry-After", FPM_HTTP_RETRY_AFTER);
		if (body) {
			evbuffer_add_printf(body, "<HTML><HEAD>\n<TITLE>503 Service Unavailable</TITLE>\n"
				"</HEAD><BODY>\n<H1>Service Unavailable</H1>\n</BODY></HTML>\n");
			evhttp_send_reply(c->req, FPM_HTTP_SERVICE_UNAVAIL, "Service Unavailable", body);
			evbuffer_free(body);
		} else {
			evhttp_send_reply(c->req, FPM_HTTP_SERVICE_UNAVAIL, "Service Unavailable", NULL);
		}
	}
	fpm_http_log_response(c->gw, c->req, c->remote_addr[0] ? c->remote_addr : c->peer_addr,
		c->remote_user, c->status, c->bytes_out,
		c->log_target ? c->log_target : c->target->pool);	/* #389 */
	fpm_http_conn_free(c);
}

/* Answers one queued request 502 because the connection to its target could
 * not even be opened (issue #465); the real reason is the log line, naming the
 * target's address. Same unlink-first contract as fpm_http_reject_queued(). */
static void fpm_http_reject_unreachable(fpm_http_conn *c, int err)
{
	zlog(ZLOG_WARNING, "[pool %s] http: cannot connect to target '%s' (%s) target=%s",
		c->gw->pool, c->target->listen_address, strerror(err), c->target->pool);
	fpm_http_finish(c, 1);
}

/* http.pool_full_policy = wait (issue #309): this request has been queued for
 * http.pool_full_wait_ms. The bound is on the wait, not on the request -- a
 * request already handed to an upstream has left gw->waiting and had its
 * timer freed, so this only ever fires for a connection still linked into
 * the queue.
 *
 * This does free the timer from inside the timer's own callback, by way of
 * fpm_http_conn_free(). That is the case libevent documents as safe -- a
 * non-persistent event is already non-pending by the time its callback runs
 * -- not the case issue #132 is open about, which is a persistent socket
 * event freed from its own read callback. */
static void fpm_http_wait_expired(evutil_socket_t fd, short what, void *arg)
{
	fpm_http_conn *c = arg;

	(void) fd;
	(void) what;
	TAILQ_REMOVE(&c->target->waiting, c, link);
	c->queued = 0;
	fpm_http_reject_queued(c);
}

/* http.pool_full_policy = wait (issue #309): how many requests are on this
 * TARGET's queue right now. Walked rather than counted in a field: the walk is
 * bounded by the cap it is compared against, it only runs when the pool is
 * already full, and a counter would have to be kept correct at every removal
 * site instead of at this one call site (see issue #107 about exactly that
 * kind of bookkeeping going wrong). The cap is per target since issue #340,
 * for the same reason the budget is: a queue belongs to the pool whose workers
 * will drain it. */
static unsigned fpm_http_waiting_len(struct fpm_http_target_s *t)
{
	fpm_http_conn *c;
	unsigned n = 0;

	TAILQ_FOREACH(c, &t->waiting, link) {
		n++;
	}
	return n;
}

/* One dispatch round for ONE target (issue #340). Split out of
 * fpm_http_pump_once() unchanged except for what it reads the queue, the
 * connection list and the budget from: a target that is full must not stop the
 * next target's dispatch, which is exactly what a single shared loop would do
 * -- the http.pool_full_policy = wait early return below would leave every
 * other target's queue standing. */
static void fpm_http_pump_target(struct fpm_http_target_s *t)
{
	struct fpm_http_gateway_s *gw = t->gw;

	while (!TAILQ_EMPTY(&t->waiting)) {
		fpm_http_upstream *up, *idle = NULL;
		fpm_http_conn *c;
		smart_str out;

		TAILQ_FOREACH(up, &t->upstreams, link) {
			if (!up->busy) {
				idle = up;
				break;
			}
		}
		if (!idle) {
			t->connect_errno = 0;
			idle = t->ops->connect(t);
		}
		if (!idle && t->connect_errno
			&& !(gw->wait_policy == FPM_HTTP_POOL_FULL_WAIT && t->nupstreams > 0)) {
			/* Not a full pool: the connection itself failed (the target's
			 * socket is gone, refused, not accessible). Waiting would never
			 * help when nothing is in flight -- no upstream will be released
			 * to pump the queue again -- so every queued request gets the
			 * real reason in the log and a 502. With http.pool_full_policy =
			 * wait and requests still in flight, the queue is left to that
			 * policy: a release re-pumps it and the wait timers bound it. */
			int err = t->connect_errno;

			while (!TAILQ_EMPTY(&t->waiting)) {
				fpm_http_conn *w = TAILQ_FIRST(&t->waiting);

				TAILQ_REMOVE(&t->waiting, w, link);
				w->queued = 0;
				fpm_http_reject_unreachable(w, err);
			}
			return;
		}
		if (!idle) {
			/* The pool is FULL for this gateway: every upstream connection it
			 * holds is busy, and the shared budget says no new one may be
			 * opened. Answer 503 + Retry-After instead of queueing towards a
			 * later END_REQUEST (task 031): the wait would be unbounded and
			 * invisible to the client, and a full pool is a transient
			 * condition worth signalling -- a broken pool (no answer from an
			 * accepted connection) is 502 instead, see fpm_http_finish().
			 * evhttp_send_error() cannot be used here: it CLEARS the output
			 * headers (libevent's evhttp_send_page_), which would strip the
			 * Retry-After this answer exists to send.
			 *
			 * The entry is unlinked before it is answered, not left for
			 * fpm_http_conn_free() to unlink on the way out (issue #107).
			 * Both orders are correct today, but this one is correct for a
			 * reason visible in the loop: a loop that frees the element it
			 * just read from the list and then re-reads TAILQ_FIRST() is only
			 * safe as long as `queued` is set on every entry in `waiting`,
			 * which is maintained at the declaration's distance from here.
			 * clang-tidy (clang-analyzer-unix.Malloc) reported this drain
			 * loop as a use-after-free and it was right about the shape,
			 * wrong about the flag.
			 *
			 * Unlinking first is also the safer order under re-entry:
			 * evhttp_send_reply() below can drive the connection close
			 * callback, and fpm_http_client_closed() -> fpm_http_conn_free()
			 * would otherwise remove and free an entry this loop still holds
			 * a pointer to.
			 *
			 * http.pool_full_policy = wait (issue #309): the one branch that
			 * makes this gateway a fast-fail, and "wait" is this branch not
			 * taken. The queue stays exactly as it is -- every entry on it
			 * already carries the timer that bounds its own stay, and this
			 * function runs again the moment an upstream is released (see
			 * fpm_http_pump()), which is what eventually empties it. Nothing
			 * else about the queue cap changes: it was already applied when
			 * the request was enqueued, not here. */
			if (gw->wait_policy == FPM_HTTP_POOL_FULL_WAIT) {
				return;
			}
			while (!TAILQ_EMPTY(&t->waiting)) {
				c = TAILQ_FIRST(&t->waiting);
				TAILQ_REMOVE(&t->waiting, c, link);
				c->queued = 0;
				fpm_http_reject_queued(c);
			}
			return;
		}

		c = TAILQ_FIRST(&t->waiting);
		TAILQ_REMOVE(&t->waiting, c, link);
		c->queued = 0;
		/* http.pool_full_policy = wait (issue #309): measured here, at the
		 * moment the request stops waiting, and reported in
		 * fpm_http_start_reply() -- not at the end of the request, which
		 * would fold the script's own runtime into the wait figure. */
		if (c->wait_timer) {
			struct timeval now, spent;

			event_free(c->wait_timer);
			c->wait_timer = NULL;
			evutil_gettimeofday(&now, NULL);
			evutil_timersub(&now, &c->wait_since, &spent);
			c->queue_wait_ms = (long) spent.tv_sec * 1000 + spent.tv_usec / 1000;
		}
		c->upstream = idle;
		idle->busy = 1;
		idle->req_written = idle->reply_seen = 0;
		idle->current = c;
		if (gw->idle_ms > 0 && !idle->connecting) {
			event_add(idle->ev_read, NULL);		/* drop the idle deadline for the duration of the request */
		}
		/* The buffer is moved out of `c` BEFORE the write, and neither `c` nor
		 * `idle` is touched after it. fpm_http_upstream_write() can fail
		 * synchronously -- fpm_http_upstream_flush() on a hard write error
		 * calls fpm_http_upstream_fail() -> fpm_http_finish(up->current) ->
		 * fpm_http_conn_free(c), which frees c->out and c itself, and
		 * fpm_http_upstream_drop() frees `idle`. Freeing a detached local is
		 * the only order that survives that; the previous one released
		 * c->out.s twice and read freed memory to do it. Issue #129. */
		out = c->out;
		memset(&c->out, 0, sizeof(c->out));
		fpm_http_upstream_write(idle, ZSTR_VAL(out.s), ZSTR_LEN(out.s));
		smart_str_free(&out);
	}
}

/* One dispatch round over every target. The order is the table's -- targets[0]
 * first -- and it does not matter: each target's queue is drained against its
 * own connections and its own budget, so no target can consume another's
 * capacity by being looked at first. Issue #389: the operator targets are
 * pumped here too; they are kept out of gw->targets (so they never appear in
 * the #341 metrics page) but their requests queue and dispatch exactly like a
 * routed target's. */
static void fpm_http_pump_once(struct fpm_http_gateway_s *gw)
{
	unsigned i;

	for (i = 0; i < gw->ntargets; i++) {
		fpm_http_pump_target(&gw->targets[i]);
	}
	for (i = 0; i < gw->noperator_targets; i++) {
		fpm_http_pump_target(&gw->operator_targets[i]);
	}
}

/* Hands waiting requests to free connections, opening new ones up to this
 * process' share. Re-entrant: the dispatch can fail synchronously and the
 * failure path calls back in here, see gw->pumping. */
void fpm_http_pump(struct fpm_http_gateway_s *gw)
{
	if (gw->pumping) {
		gw->pump_again = 1;
		return;
	}
	gw->pumping = 1;
	do {
		gw->pump_again = 0;
		fpm_http_pump_once(gw);
	} while (gw->pump_again);
	gw->pumping = 0;
}

/* Issue #490: process-local index for the nodes introduced by #390. evcon is
 * the exact live object identity and is available both when a request arrives
 * and when its close callback fires. A mixed pointer hash keeps aligned heap
 * addresses from bunching into the same buckets; the explicit links in each node
 * make close an exact removal rather than another lookup. */
#define FPM_HTTP_CLIENT_INDEX_INITIAL_BUCKETS 64U

static size_t fpm_http_client_hash(const struct evhttp_connection *evcon)
{
	uint64_t x = (uintptr_t) evcon;

	/* splitmix64's finalizer: evcon addresses are aligned, so using low bits
	 * directly would systematically discard the bits that vary. */
	x += UINT64_C(0x9e3779b97f4a7c15);
	x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
	return (size_t) (x ^ (x >> 31));
}

static struct fpm_http_client_s *fpm_http_client_index_find(
	struct fpm_http_client_index_s *index, const struct evhttp_connection *evcon)
{
	struct fpm_http_client_s *cl;
	size_t bucket;

	if (!index->bucket_count) {
		return NULL;
	}
	bucket = fpm_http_client_hash(evcon) & (index->bucket_count - 1);
	for (cl = index->buckets[bucket]; cl; cl = cl->hash_next) {
		if (cl->evcon == evcon) {
			return cl;
		}
	}
	return NULL;
}

static bool fpm_http_client_index_grow(struct fpm_http_client_index_s *index)
{
	struct fpm_http_client_s **buckets;
	size_t bucket_count = index->bucket_count
		? index->bucket_count * 2 : FPM_HTTP_CLIENT_INDEX_INITIAL_BUCKETS;
	size_t i;

	if (bucket_count < index->bucket_count || bucket_count > SIZE_MAX / sizeof(struct fpm_http_client_s *)) {
		return false;
	}
	buckets = calloc(bucket_count, sizeof(struct fpm_http_client_s *));
	if (!buckets) {
		return false;
	}
	for (i = 0; i < index->bucket_count; i++) {
		struct fpm_http_client_s *cl = index->buckets[i];

		while (cl) {
			struct fpm_http_client_s *next = cl->hash_next;
			size_t bucket = fpm_http_client_hash(cl->evcon) & (bucket_count - 1);

			cl->hash_bucket = bucket;
			cl->hash_prev = NULL;
			cl->hash_next = buckets[bucket];
			if (buckets[bucket]) {
				buckets[bucket]->hash_prev = cl;
			}
			buckets[bucket] = cl;
			cl = next;
		}
	}
	free(index->buckets);
	index->buckets = buckets;
	index->bucket_count = bucket_count;
	return true;
}

static bool fpm_http_client_index_insert(struct fpm_http_client_index_s *index,
	struct fpm_http_client_s *cl)
{
	struct fpm_http_client_s *head;

	if (!index->bucket_count && !fpm_http_client_index_grow(index)) {
		return false;
	}
	/* Grow before crossing 75%. If that allocation fails, the old table remains
	 * valid and the request can still be tracked, albeit above the preferred
	 * load factor. Only the initial table is a hard tracking failure. */
	if (index->count >= index->bucket_count - index->bucket_count / 4) {
		(void) fpm_http_client_index_grow(index);
	}
	cl->hash_bucket = fpm_http_client_hash(cl->evcon) & (index->bucket_count - 1);
	head = index->buckets[cl->hash_bucket];
	cl->hash_prev = NULL;
	cl->hash_next = head;
	if (head) {
		head->hash_prev = cl;
	}
	index->buckets[cl->hash_bucket] = cl;
	index->count++;
	return true;
}

static void fpm_http_client_index_remove(struct fpm_http_client_index_s *index,
	struct fpm_http_client_s *cl)
{
	if (cl->hash_prev) {
		cl->hash_prev->hash_next = cl->hash_next;
	} else {
		index->buckets[cl->hash_bucket] = cl->hash_next;
	}
	if (cl->hash_next) {
		cl->hash_next->hash_prev = cl->hash_prev;
	}
	cl->hash_prev = NULL;
	cl->hash_next = NULL;
	if (index->count) {
		index->count--;
	}
}

/* Issue #390: claims (or finds) the gateway-process node for one accepted
 * connection and registers the connection's one close callback on it. Called
 * first thing in fpm_http_request() and in fpm_http_plain_request(), so it runs
 * for every request however it is answered, and the process's connections_open
 * gauge therefore counts a connection whether it proxied, pinged, redirected or
 * 404ed. Incrementing it exactly once per connection is why the node is keyed
 * by evcon instead of doing this in the bevcb: evhttp hands the bevcb a
 * bufferevent, not the connection, and the connection is what outlives a
 * keep-alive request. NULL when there is no evcon or on OOM: the request still
 * works, its connection is just not counted. */
static struct fpm_http_client_s *fpm_http_client_track(struct fpm_http_gateway_s *gw,
	struct evhttp_connection *evcon)
{
	struct fpm_http_client_s *cl;

	if (!gw || !evcon) {
		return NULL;
	}
	cl = fpm_http_client_index_find(&gw->client_index, evcon);
	if (cl) {
		return cl;
	}
	cl = calloc(1, sizeof(*cl));
	if (!cl) {
		return NULL;
	}
	cl->gw = gw;
	cl->evcon = evcon;
	if (!fpm_http_client_index_insert(&gw->client_index, cl)) {
		free(cl);
		return NULL;
	}
	if (gw->gauges) {
		fpm_http_counter_incr(&gw->gauges[0]);
	}
	evhttp_connection_set_closecb(evcon, fpm_http_client_closed, cl);
	return cl;
}

/* The client connection is gone, with or without a request in flight. Stops
 * writing to it, lets the pool finish so the connection stays usable when there
 * is one, and releases the connection's node plus this process's own
 * connections_open cell. arg is the fpm_http_client_s registered by
 * fpm_http_client_track(), not the request: a connection that closed between
 * two keep-alive requests has no request to point at, and the old per-request
 * callback would have leaked its increment instead. */
static void fpm_http_client_closed(struct evhttp_connection *evcon, void *arg)
{
	struct fpm_http_client_s *cl = arg;
	struct fpm_http_gateway_s *gw = cl->gw;
	fpm_http_conn *c = cl->c;

	/* One close notification owns this node. Clearing the slot first also keeps
	 * a nested libevent close path from seeing the same callback as current. */
	evhttp_connection_set_closecb(evcon, NULL, NULL);
	/* Issue #593: before the connection's fd goes, so the one-shot watcher is
	 * never registered on a closed descriptor. */
	if (cl->ka_timer) {
		event_free(cl->ka_timer);
		cl->ka_timer = NULL;
	}
	if (cl->ka_watch) {
		event_free(cl->ka_watch);
		cl->ka_watch = NULL;
	}
	if (c) {
		/* Issue #596: the upstream was paused for this client's sake. The
		 * client is gone, so the worker's remaining output is read and dropped
		 * and the pinned connection comes back, the same as when a client
		 * leaves a response that is not paused. */
		fpm_http_response_resume(c);
		c->evcon = NULL;
		c->client = NULL;
		cl->c = NULL;
		if (c->upstream) {
			c->upstream->current = NULL;
			c->upstream = NULL;
		}
	}
	/* Remove before fpm_http_conn_free(): timer teardown or a future request
	 * cleanup can re-enter, and must not find this connection still indexed. */
	fpm_http_client_index_remove(&gw->client_index, cl);
	if (gw->gauges) {
		fpm_http_counter_decr(&gw->gauges[0]);
	}
	if (c) {
		fpm_http_conn_free(c);
	}
	free(cl);
}

/* ---------------------------------------------------------------- client limits after the first request (issue #593) */

/* The deadline for the next request expired, or the client stalled. Shrinks the
 * connection's own timeouts to (almost) zero so evhttp closes it through its
 * error path -- the same, and only safe, way fpm_http_read_deadline_fire()
 * ends a connection (a bufferevent_free() underneath evhttp is a
 * use-after-free, issue #90). `cl->evcon` is valid here: the close callback
 * frees this timer before evhttp lets go of the connection. */
static void fpm_http_client_idle_fire(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_http_client_s *cl = arg;
	static const struct timeval now = {0, 1};

	(void) fd; (void) what;
	if (cl->ka_watch) {
		event_del(cl->ka_watch);
	}
	bufferevent_set_timeouts(evhttp_connection_get_bufferevent(cl->evcon), &now, &now);
}

/* The first byte of the next request arrived. From here the client is
 * delivering a request, and that is http.read_timeout's job: the same
 * absolute budget a first request gets, whatever the spacing of its bytes. The
 * keep-alive timer is replaced, not stacked; it is created here when
 * http.keepalive_timeout = 0 left it unarmed. With http.read_timeout = 0 the
 * keep-alive timer simply keeps running, so the connection is still bounded.
 * Limit: bytes of the next request that arrive together with the previous one
 * are already in the bufferevent input, so this watcher never fires for them;
 * such a connection is bounded by http.keepalive_timeout, not by
 * http.read_timeout. */
static void fpm_http_client_idle_byte(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_http_client_s *cl = arg;

	(void) fd; (void) what;
	if (cl->gw->read_timeout_ms <= 0) {
		return;
	}
	if (!cl->ka_timer) {
		cl->ka_timer = event_new(cl->gw->base, -1, EV_TIMEOUT, fpm_http_client_idle_fire, cl);
	}
	if (cl->ka_timer) {
		event_add(cl->ka_timer, &cl->gw->read_timeout);
	}
}

/* The response is complete and the connection stays open for another request:
 * start the keep-alive clock. If evhttp is about to close the connection
 * instead (Connection: close, an error reply), the close callback frees both
 * events a moment later -- nothing here holds a reference to the connection,
 * which is the reason this lives on the client node and not on the
 * struct fpm_http_read_deadline_s bufferevent reference. A reference held
 * past a close would keep the fd, and a "Connection: close" client would wait
 * for the whole keep-alive timeout to see EOF. */
static void fpm_http_client_request_done(struct evhttp_request *req, void *arg)
{
	struct fpm_http_client_s *cl = arg;
	struct fpm_http_gateway_s *gw = cl->gw;
	evutil_socket_t fd;

	(void) req;
	if ((gw->keepalive_timeout_ms <= 0 && gw->read_timeout_ms <= 0) || !cl->evcon) {
		return;
	}
	/* Keep-alive 0 means unlimited idle, but a later request is still bounded by
	 * http.read_timeout: only the first-byte watcher is armed then. */
	if (gw->keepalive_timeout_ms > 0) {
		if (!cl->ka_timer) {
			cl->ka_timer = event_new(gw->base, -1, EV_TIMEOUT, fpm_http_client_idle_fire, cl);
		}
		if (!cl->ka_timer) {
			return;	/* OOM: this connection is not bounded, the gateway still works */
		}
		event_add(cl->ka_timer, &gw->keepalive_timeout);
	}
	if (gw->read_timeout_ms <= 0) {
		return;
	}
	fd = bufferevent_getfd(evhttp_connection_get_bufferevent(cl->evcon));
	if (fd < 0) {
		return;
	}
	if (!cl->ka_watch) {
		cl->ka_watch = event_new(gw->base, fd, EV_READ, fpm_http_client_idle_byte, cl);
	}
	if (cl->ka_watch) {
		event_add(cl->ka_watch, NULL);
	}
}

/* Called first thing for every request dispatched on a connection, from both
 * listeners. A request reaching a gencb has been read completely, so the
 * keep-alive clock (and the first-byte watcher) are spent; the completion hook
 * restarts them when this request's response is done. Also puts the client
 * write stall limit on the connection (http.write_timeout): a bufferevent write
 * timeout runs only while output is pending and restarts on every byte the
 * client takes, so it is a stall timer -- unlike a read timeout it does not
 * cut a slow upstream, because nothing is pending to the client then. The read
 * side is passed as NULL on purpose: evhttp's read timeout is an idle timer
 * and would cut exactly that slow upstream (see the bevcb comment). */
static void fpm_http_client_request_begin(struct fpm_http_gateway_s *gw,
	struct fpm_http_client_s *cl, struct evhttp_request *req)
{
	if (!gw || !cl) {
		return;
	}
	if (cl->ka_timer) {
		event_del(cl->ka_timer);
	}
	if (cl->ka_watch) {
		event_del(cl->ka_watch);
	}
	evhttp_request_set_on_complete_cb(req, fpm_http_client_request_done, cl);
	if (gw->write_timeout_ms > 0 && cl->evcon) {
		bufferevent_set_timeouts(evhttp_connection_get_bufferevent(cl->evcon), NULL, &gw->write_timeout);
	}
}

/* ------------------------------------------------------------------------ *
 * Responses the gateway provides ITSELF, without occupying a worker.
 *
 * This is the single point for all such cases. Today: static files; later, add
 * the ACME challenge (/.well-known/acme-challenge/) and /status here. Do not
 * turn these into ad-hoc if statements in fpm_http_request — each case needs
 * exactly the same steps: resolve the path, check containment in the document
 * root, and respond without FastCGI.
 * ------------------------------------------------------------------------ */

/* Resolved document root, once per gateway process. */
static const char *fpm_http_docroot_real(struct fpm_http_gateway_s *gw)
{
	static char resolved[MAXPATHLEN];
	static int done = 0;

	if (!done) {
		done = 1;
		if (!realpath(gw->docroot, resolved)) {
			resolved[0] = '\0';
		}
	}

	return resolved[0] ? resolved : NULL;
}

/* Validates http.front_controller once, in the master, before the first gateway
 * fork -- fork()'s COW then hands every gateway process gw->front_controller_ok
 * already decided, exactly like fpm_tls_http_validate() decides TLS cert/key
 * problems at pool-validation time instead of at first request (task 018 gap 2:
 * this used to be a function-level static evaluated lazily on the first request
 * of each gateway process; that was fine only because each gateway process
 * serves exactly one pool, and it reported a misconfiguration late). The value
 * is admin config, not request input, but it still goes through the same
 * realpath()-under-docroot check as a static file (see fpm_http_serve_static):
 * a symlink can put a perfectly innocent-looking path outside the document
 * root, and there is no reason to trust config more than we trust the
 * filesystem. When the front controller is not deployed yet, realpath() has
 * nothing to resolve -- the fallback is still enabled in that case, so a
 * request that reaches it gets the worker's usual "File not found" instead of
 * silently behaving as if the option were unset.
 *
 * gw->docroot and gw->front_controller must already be set (fpm_http_init_pool_ex()
 * calls this right after fpm_http_gateway_settings()); the master and every
 * gateway child share the same filesystem view for this pool (no chroot/chdir
 * happens between here and fpm_http_gateway_run()), so resolving the document
 * root here is exactly as valid as resolving it later in the child. */
void fpm_http_front_controller_validate(struct fpm_http_gateway_s *gw)
{
	const char *fc = gw->front_controller;

	gw->front_controller_ok = 0;
	if (fc && *fc) {
		const char *root = fpm_http_docroot_real(gw);
		char candidate[MAXPATHLEN], resolved[MAXPATHLEN];

		if (!root) {
			zlog(ZLOG_WARNING, "[pool %s] http: http.front_controller fallback disabled, document root does not resolve", gw->pool);
		} else if ((size_t)snprintf(candidate, sizeof(candidate), "%s%s", gw->docroot, fc) >= sizeof(candidate)) {
			zlog(ZLOG_WARNING, "[pool %s] http: http.front_controller '%s' is too long, fallback disabled", gw->pool, fc);
		} else if (!realpath(candidate, resolved)) {
			gw->front_controller_ok = 1;	/* not deployed yet: still enable, see the comment above */
		} else {
			size_t root_len = strlen(root);

			if (!strncmp(resolved, root, root_len) && (!resolved[root_len] || resolved[root_len] == '/')) {
				gw->front_controller_ok = 1;
			} else {
				zlog(ZLOG_WARNING, "[pool %s] http: http.front_controller '%s' resolves outside the document root, fallback disabled", gw->pool, fc);
			}
		}
	}
}

/* cmd == EVHTTP_REQ_GET or EVHTTP_REQ_HEAD, and http.static is on: fills in
 * *script_missing with what realpath()/fstat() below find out about the same
 * path fpm_http_build_request() would use as SCRIPT_FILENAME (0 = exists as a
 * regular file, 1 = confirmed missing or a directory), so that build_request
 * can skip its own stat() for the one case this function already paid for.
 * -1 (unchanged from the caller's initial value) means this function never
 * reached that check -- the caller still doesn't know. */
struct fpm_http_static_log_ctx {
	struct fpm_http_gateway_s *gw;
	struct evhttp_request *req;
	const char *remote_addr;
};

static void fpm_http_static_log_response(void *ctx, int status, size_t bytes)
{
	struct fpm_http_static_log_ctx *c = ctx;

	fpm_http_log_response(c->gw, c->req, c->remote_addr, NULL, status, bytes, NULL);
}

static int fpm_http_serve_static(struct fpm_http_gateway_s *gw, struct evhttp_request *req,
		const char *path, size_t path_len, const char *remote_addr, int *script_missing)
{
	struct fpm_http_static_log_ctx ctx = { gw, req, remote_addr };
	struct fpm_http_static st;

	memset(&st, 0, sizeof(st));
	st.pool = gw->pool;
	st.root = fpm_http_docroot_real(gw);	/* NULL when it does not resolve: nothing is served */
	st.log = fpm_http_static_log_response;
	st.log_ctx = &ctx;

	return fpm_http_static_serve(&st, req, path, path_len, script_missing);
}

static const char fpm_http_acme_prefix[] = "/.well-known/acme-challenge/";

/* HTTP-01, answered from the shared challenge state (fpm_acme_challenge.h).
 * Returns 1 when this request has been answered, 0 when `path` is not in the
 * challenge namespace at all.
 *
 * The whole namespace is answered here: an unknown token is a 404 from this
 * function and never reaches a worker or the disk. That is what makes
 * criterion 3 of issue #48 hold -- a file physically present under the
 * document root at this path cannot shadow the answer or leak into it,
 * because fpm_http_serve_static() is never consulted for these paths -- and
 * it is why this does not depend on http.static (criterion 4): a key
 * authorization is not a file.
 *
 * The token is whatever follows the prefix, taken as one flat opaque
 * segment. A '/' in it means the CA asked for something that is not a token,
 * so it is a 404 rather than a lookup; nothing here is ever turned into a
 * path, which is criterion 5 with no filesystem involved to get wrong. */
static int fpm_http_serve_acme_challenge(struct fpm_http_gateway_s *gw, struct evhttp_request *req,
		const char *path, size_t path_len, const char *remote_addr)
{
	static const size_t prefix_len = sizeof(fpm_http_acme_prefix) - 1;
	char keyauth[FPM_ACME_CHALLENGE_KEYAUTH_MAX];
	struct evkeyvalq *out;
	const char *token;
	ssize_t len;
	int cmd;

	if (path_len < prefix_len || memcmp(path, fpm_http_acme_prefix, prefix_len) != 0) {
		return 0;
	}
	token = path + prefix_len;

	cmd = evhttp_request_get_command(req);
	if (cmd != EVHTTP_REQ_GET && cmd != EVHTTP_REQ_HEAD) {
		/* Still answered locally: this namespace belongs to ACME whatever the
		 * method, and forwarding a POST here to the application would expose
		 * a path the application never routes. */
		fpm_http_log_response(gw, req, remote_addr, NULL, 405, 0, NULL);
		evhttp_send_error(req, 405, "Method Not Allowed");
		return 1;
	}

	len = *token && !strchr(token, '/')
		? fpm_acme_challenge_lookup(token, keyauth, sizeof(keyauth))
		: -1;
	if (len < 0) {
		fpm_http_log_response(gw, req, remote_addr, NULL, HTTP_NOTFOUND, 0, NULL);
		evhttp_send_error(req, HTTP_NOTFOUND, "ACME challenge is not provisioned");
		return 1;
	}

	out = evhttp_request_get_output_headers(req);
	/* RFC 8555 section 8.3: the response body is the key authorization and
	 * nothing else -- no trailing newline, and text/plain rather than the
	 * charset-qualified type used for static .txt files, because the CA
	 * compares bytes. */
	evhttp_add_header(out, "Content-Type", "text/plain");
	if (cmd == EVHTTP_REQ_HEAD) {
		char content_length[32];

		snprintf(content_length, sizeof(content_length), "%zd", len);
		evhttp_add_header(out, "Content-Length", content_length);
		fpm_http_log_response(gw, req, remote_addr, NULL, HTTP_OK, 0, NULL);
		evhttp_send_reply(req, HTTP_OK, "OK", NULL);
		return 1;
	}
	{
		struct evbuffer *body = evbuffer_new();

		if (!body || evbuffer_add(body, keyauth, (size_t) len) < 0) {
			if (body) {
				evbuffer_free(body);
			}
			fpm_http_log_response(gw, req, remote_addr, NULL, FPM_HTTP_BAD_GATEWAY, 0, NULL);
			evhttp_send_error(req, FPM_HTTP_BAD_GATEWAY, "Bad Gateway");
			return 1;
		}
		fpm_http_log_response(gw, req, remote_addr, NULL, HTTP_OK, (size_t) len, NULL);
		evhttp_send_reply(req, HTTP_OK, "OK", body);
		evbuffer_free(body);
	}
	return 1;
}

/* ping.path, answered directly by the gateway process -- issue #382. Called
 * from fpm_http_request(), first of all, so this runs BEFORE the operator
 * namespace (#389), BEFORE ACME, BEFORE the static
 * lookup (a stray docroot/ping file must not shadow the probe) and before
 * fpm_http_build_request(), routing, the queue and
 * the FastCGI connection: no child, no queue slot, no scoreboard entry,
 * pm.max_requests or queue counter is ever touched by a locally answered
 * ping. It is not a request of the pool.
 *
 * Matched against the origin-form path of the request-target
 * (fpm_http_raw_path(): an absolute-form target is reduced to its path; not the
 * percent-decoded path fpm_http_static_decode_path() produces for ACME/static
 * below), with any query string cut off and the whole path compared so that
 * "/pings" is not "/ping" -- verbatim the matcher http-direct already uses,
 * fpm_http_direct_ops_try_local() in fpm_http_direct_ops.c. No
 * percent-decoding: ping.path is a literal in the pool file and upstream
 * matches it literally too, so "/%70ing" is not a way past a proxy rule
 * written against the documented spelling. */
static int fpm_http_serve_ping(struct fpm_http_gateway_s *gw, struct evhttp_request *req, const char *remote_addr)
{
	char path[512];
	struct evkeyvalq *out;
	struct evbuffer *body;
	size_t bytes;

	if (!gw->ping_path || !fpm_http_raw_path(req, path, sizeof(path))) {
		return 0;
	}
	if (strcmp(path, gw->ping_path) != 0) {
		return 0;
	}

	body = evbuffer_new();
	if (!body || evbuffer_add_printf(body, "%s", gw->ping_response ? gw->ping_response : "pong") < 0) {
		if (body) {
			evbuffer_free(body);
		}
		fpm_http_log_response(gw, req, remote_addr, NULL, FPM_HTTP_BAD_GATEWAY, 0, NULL);
		evhttp_send_error(req, FPM_HTTP_BAD_GATEWAY, "Bad Gateway");
		return 1;
	}

	out = evhttp_request_get_output_headers(req);
	evhttp_add_header(out, "Content-Type", "text/plain");
	/* Same three headers upstream's fpm_status.c sends for ping/status: a
	 * liveness probe a proxy is free to cache is a liveness probe that lies. */
	evhttp_add_header(out, "Expires", "Thu, 01 Jan 1970 00:00:00 GMT");
	evhttp_add_header(out, "Cache-Control", "no-cache, no-store, must-revalidate, max-age=0");
	bytes = evbuffer_get_length(body);
	fpm_http_log_response(gw, req, remote_addr, NULL, HTTP_OK, bytes, NULL);
	evhttp_send_reply(req, HTTP_OK, "OK", body);
	evbuffer_free(body);
	return 1;
}

/* Returns 1 when the gateway answered on its own; 0 to hand the request to a
 * worker. *script_missing carries fpm_http_serve_static()'s realpath() result
 * out (see the comment there) so fpm_http_build_request() can reuse it. */
static int fpm_http_try_local(struct fpm_http_gateway_s *gw, struct evhttp_request *req, const char *remote_addr, int *script_missing)
{
	char *path;
	size_t path_len;
	int answered = 0;

	/* Issue #389: ping.path is no longer answered here; fpm_http_request()
	 * calls fpm_http_serve_ping() before this, because the operator namespace
	 * must be checked after ping and before everything else in this function.
	 * See fpm_http_serve_ping(). */

	/* Not gated on gw->static_files: the ACME challenge below is not a
	 * static file, and http.static = 0 must not switch it off (issue #48,
	 * criterion 4). The static branch keeps its own check further down. */
	path = fpm_http_static_decode_path(req, &path_len);
	if (!path) {
		return 0;			/* let fpm_http_build_request produce the 400 */
	}

	/* Fixed-path things first, files from disk only at the end -- the order
	 * docs/NOTES.md section 3l reserved for this hook, now that ACME uses it. */
	answered = fpm_http_serve_acme_challenge(gw, req, path, path_len, remote_addr);
	if (!answered && gw->static_files) {
		answered = fpm_http_serve_static(gw, req, path, path_len, remote_addr, script_missing);
	}

	free(path);

	return answered;
}


/* The plain redirect companion's share of the local-answer hook. Decodes the
 * path the same way fpm_http_try_local() does -- a percent-encoded prefix must
 * not slip past the namespace check -- and hands it to the one responder. */
static int fpm_http_plain_try_acme(struct evhttp_request *req, void *arg)
{
	struct fpm_http_gateway_s *gw = arg;
	const struct evhttp_uri *decoded_uri = evhttp_request_get_evhttp_uri(req);
	const char *raw_path = decoded_uri ? evhttp_uri_get_path(decoded_uri) : NULL;
	struct evhttp_connection *evcon = evhttp_request_get_connection(req);
	char *peer_addr = NULL;
	ev_uint16_t peer_port = 0;
	size_t path_len;
	char *path;
	int answered;

	if (!gw || !raw_path || !*raw_path) {
		return 0;
	}
	path = evhttp_uridecode(raw_path, 0, &path_len);
	if (!path) {
		return 0;
	}
	if (path_len != strlen(path) || path[0] != '/') {
		free(path);
		return 0;
	}
	if (evcon) {
		evhttp_connection_get_peer(evcon, &peer_addr, &peer_port);
	}
	answered = fpm_http_serve_acme_challenge(gw, req, path, path_len, peer_addr);
	free(path);
	return answered;
}

void fpm_http_plain_request(struct evhttp_request *req, void *arg)
{
	struct fpm_http_gateway_s *gw = arg;
	struct evhttp_connection *evcon = evhttp_request_get_connection(req);
	const char *host = evhttp_find_header(evhttp_request_get_input_headers(req), "Host");
	const char *uri = evhttp_request_get_uri(req);
	struct evkeyvalq *headers = evhttp_request_get_output_headers(req);
	char *location;
	char *redirect_host = NULL;
	char authority[FPM_HTTP_AUTHORITY_MAX];
	smart_str target = {0};
	size_t len;

	fpm_http_normalize_target(req);
	/* Issue #390 review: this listener bypassed all of the gateway's own
	 * accounting. Every plain request is answered locally -- the ACME
	 * challenge, the 308 redirect, the NO_CERT 503, a 400 -- and it is the
	 * ONLY listener serving in NO_CERT, so an uncounted plain path reported
	 * baseline 0 while answering the CA and broke the rule that the baseline
	 * equals the sum of the target rows. Count it exactly like the main
	 * listener: one accepted request, one local ("-") answer, and the
	 * connection into this process's connections_open. ping.path is not
	 * served here, so ping_total is untouched. */
	if (gw && gw->counters) {
		fpm_http_counter_incr(&gw->counters->requests_total);
	}
	fpm_http_client_request_begin(gw, fpm_http_client_track(gw, evcon), req);
	if (gw && gw->read_timeout_ms > 0) {
		fpm_http_read_deadline_disarm(gw, evcon ? evhttp_connection_get_bufferevent(evcon) : NULL);
	}
	fpm_http_count_local(gw);

	/* HTTP-01 before anything else, including the redirect: the CA speaks
	 * plain HTTP on purpose and must not be sent to :443 for a certificate
	 * that does not exist yet (docs/NOTES.md section 3l, the NO_CERT state).
	 * This companion has no document root and no worker, so the challenge is
	 * the only thing it can answer -- and, with the redirect skipped, the
	 * only thing in this namespace it ever answers, provisioned or not. */
	if (fpm_http_plain_try_acme(req, arg)) {
		return;
	}
	{
		/* NO_CERT (issue #172 criterion 2): redirecting to https:// would
		 * send the client to a port that is refusing connections, which
		 * reads to a browser as "the site is broken" rather than "the site
		 * is not provisioned yet". 503 says the second, and Retry-After
		 * gives a well-behaved client something to act on. Nothing else is
		 * served here either -- this companion has no document root and no
		 * worker -- so the challenge answered above remains the only thing
		 * this listener ever returns before the certificate exists. */
		if (gw && !gw->tls_ready) {
			evhttp_add_header(headers, "Retry-After", "60");
			evhttp_send_error(req, HTTP_SERVUNAVAIL, "Service Unavailable");
			return;
		}
	}
	/* An absolute-form target names its own authority, which replaces Host
	 * (RFC 9112 3.2.2), and is redirected as origin-form -- otherwise the
	 * Location would be "https://h" + "http://h/x" (#534). */
	if (uri) {
		int have = fpm_http_absolute_authority(uri, authority, sizeof(authority));

		if (have < 0) {
			evhttp_send_error(req, HTTP_BADREQUEST, "Bad Request");
			return;
		}
		if (have > 0) {
			host = authority;
		}
	}
	if (uri) {
		fpm_http_origin_form(&target, req);
		smart_str_0(&target);
		uri = ZSTR_VAL(target.s);
	}
	if (!host || !*host || strchr(host, '\r') || strchr(host, '\n') || !uri) {
		evhttp_send_error(req, HTTP_BADREQUEST, "Bad Request");
		smart_str_free(&target);
		return;
	}
	if (host[0] == '[') {
		const char *end = strchr(host, ']');

		if (!end || (end[1] && end[1] != ':')) {
			evhttp_send_error(req, HTTP_BADREQUEST, "Bad Request");
			smart_str_free(&target);
			return;
		}
		redirect_host = strndup(host, (size_t)(end - host + 1));
	} else {
		const char *colon = strrchr(host, ':');

		redirect_host = colon ? strndup(host, (size_t)(colon - host)) : strdup(host);
	}
	if (!redirect_host || !*redirect_host) {
		free(redirect_host);
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Service Unavailable");
		smart_str_free(&target);
		return;
	}

	len = sizeof("https://") - 1 + strlen(redirect_host) + strlen(uri) + 1;
	location = malloc(len);
	if (!location) {
		free(redirect_host);
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Service Unavailable");
		smart_str_free(&target);
		return;
	}
	snprintf(location, len, "https://%s%s", redirect_host, uri);
	evhttp_add_header(headers, "Location", location);
	evhttp_send_reply(req, 308, "Permanent Redirect", NULL);
	free(location);
	free(redirect_host);
	smart_str_free(&target);
}

/* ------------------------------------------------------------------- routing */

/* Does `path` fall under `prefix`? Segment-aware, which is the whole
 * difference between a path prefix and a string prefix: "/api" covers "/api"
 * and "/api/x" but NOT "/apiary". "/" covers everything -- it is the one
 * prefix whose last character IS the separator, so it needs no extra rule,
 * only the one below that accepts a prefix already ending in '/'. */
static int fpm_http_prefix_covers(const char *prefix, size_t prefix_len, const char *path, size_t path_len)
{
	if (path_len < prefix_len || memcmp(path, prefix, prefix_len) != 0) {
		return 0;
	}
	if (path_len == prefix_len || prefix[prefix_len - 1] == '/') {
		return 1;
	}
	return path[prefix_len] == '/';
}

/* Which backend pool serves this request (issue #340).
 *
 * Longest matching prefix wins, and the table is already sorted longest first,
 * so the first match is the answer. On the old http weld it always found one:
 * the last row was "/", either because http.route[] claimed it or because the
 * gateway's own listener was inserted there. Issue #388's gateway has no such
 * row, so this can return NULL -- and fpm_http_request() answers that with a
 * local 404, never a forward. A request matching no route is an ordinary state
 * on a gateway, not an error.
 *
 * Matching is on the DECODED path, the same bytes SCRIPT_NAME is built from
 * (fpm_http_build_request()), so "/%61pi/x" routes exactly as "/api/x" does
 * and a percent-encoded prefix cannot slip past a route. A path that does not
 * decode at all takes the "/" row and then fails in write_request() with the
 * 400 it always produced -- the decode is not repeated for the verdict's sake.
 *
 * Runs AFTER fpm_http_try_local(), so ping.path, the ACME challenge and static
 * files are still answered by the gateway itself and are never routed
 * anywhere (issue #382 for ping.path in particular: the probe proves the
 * gateway is alive, not any target's workers). */
static struct fpm_http_target_s *fpm_http_route(struct fpm_http_gateway_s *gw, struct evhttp_request *req)
{
	const char *path;
	char *decoded;
	size_t decoded_len;
	unsigned i;
	struct fpm_http_target_s *hit;

	/* The one-row shortcut below is the old weld's: a table with a single
	 * "/" row always matches. Issue #388's gateway has no such row, and must
	 * run the walk even with one route -- so the shortcut is guarded, and the
	 * fallback row it used to reach for (the last, shortest prefix) is not a
	 * fallback on the gateway at all. */
	if (!gw->proxy_only && gw->nroutes < 2) {
		return gw->routes[0].target;
	}

	path = fpm_http_request_path(req);
	if (!path || !*path) {
		/* No path to route on: the gateway has no implicit target, so this
		 * is a local 404 (fpm_http_request() answers it). */
		return gw->proxy_only ? NULL : gw->routes[gw->nroutes - 1].target;
	}
	decoded = evhttp_uridecode(path, 0, &decoded_len);
	if (!decoded) {
		return gw->proxy_only ? NULL : gw->routes[gw->nroutes - 1].target;
	}

	hit = gw->proxy_only ? NULL : gw->routes[gw->nroutes - 1].target;
	for (i = 0; i < gw->nroutes; i++) {
		if (fpm_http_prefix_covers(gw->routes[i].prefix, gw->routes[i].prefix_len, decoded, decoded_len)) {
			hit = gw->routes[i].target;
			break;
		}
	}
	free(decoded);
	return hit;
}

/* The shared tail of request dispatch: serialize the request for c->target (its
 * transport), then either answer a synchronous write failure or enqueue and
 * pump. Split out so a routed request and an operator-forwarded one -- which
 * set c->target and c->upstream_uri differently -- cannot drift. This is the
 * block fpm_http_request() used to inline; the comments explaining the order
 * live with the code that is still here. */
void fpm_http_dispatch(struct fpm_http_gateway_s *gw, fpm_http_conn *c, int script_missing)
{
	const char *target = c->log_target ? c->log_target : c->target->pool;
	int error = c->target->ops->write_request(c, script_missing);

	if (error) {
		fpm_http_log_response(gw, c->req, c->remote_addr[0] ? c->remote_addr : c->peer_addr,
			c->remote_user, error, 0, target);
		evhttp_send_error(c->req, error, NULL);
		c->evcon = NULL;
		fpm_http_conn_free(c);
		return;
	}

	/* Issue #390: the connection's close callback is already the gauge's, set
	 * once in fpm_http_client_track(); this request only has to become its
	 * in-flight one, so a client that goes away mid-proxy (the case the old
	 * per-request closecb existed for) is still reached. */
	if (c->client) {
		c->client->c = c;
	}
	/* http.pool_full_policy = wait (issue #309): the queue cap, applied
	 * before the insert rather than left to the next pump. Before, because
	 * the answer this gives has to be immediate -- a client at the cap gets
	 * the ordinary 503 straight away, it does not wait its own wait bound
	 * out only to be rejected anyway -- and because a request that is never
	 * enqueued needs no timer and nothing to unlink. */
	if (gw->wait_policy == FPM_HTTP_POOL_FULL_WAIT && fpm_http_waiting_len(c->target) >= (unsigned) gw->wait_queue_max) {
		fpm_http_reject_queued(c);
		return;
	}
	TAILQ_INSERT_TAIL(&c->target->waiting, c, link);
	c->queued = 1;
	if (gw->wait_policy == FPM_HTTP_POOL_FULL_WAIT) {
		evutil_gettimeofday(&c->wait_since, NULL);
		c->wait_timer = evtimer_new(gw->base, fpm_http_wait_expired, c);
		if (c->wait_timer) {
			evtimer_add(c->wait_timer, &gw->wait_bound);
		}
		/* No timer means no bound on how long this request could wait, which
		 * is the one thing this policy is not allowed to be -- but leaving it
		 * queued with no timer (evtimer_new() only fails on OOM) is still
		 * safer than rejecting a request that is otherwise fine: the next
		 * successful fpm_http_pump() still dispatches it normally, same as
		 * any other queued entry. */
	}
	fpm_http_pump(gw);
}

void fpm_http_request(struct evhttp_request *req, void *arg)
{
	struct fpm_http_gateway_s *gw = arg;
	struct evhttp_connection *evcon = evhttp_request_get_connection(req);
	char *peer_addr = NULL;
	ev_uint16_t peer_port = 0;
	struct fpm_http_forwarded_result_s fwd;
	const char *effective_addr;
	struct fpm_http_client_s *client;
	fpm_http_conn *c;

	fpm_http_normalize_target(req);
	if (evcon) {
		evhttp_connection_get_peer(evcon, &peer_addr, &peer_port);
	}

	/* Issue #390: the pool's headline number -- every request the gateway
	 * accepted, whatever happens to it next. Bumped before the ACL so a denied
	 * request still counts as accepted, and once here rather than at the
	 * per-bucket sites so the total can never drift from the sum of the rows
	 * below. fpm_http_client_track() registers the connection's close callback
	 * (the connections_open gauge) at the same time. */
	if (gw->counters) {
		fpm_http_counter_incr(&gw->counters->requests_total);
	}
	client = fpm_http_client_track(gw, evcon);
	fpm_http_client_request_begin(gw, client, req);

	/* Reaching this callback means the client delivered the whole request
	 * (evhttp buffers headers AND body before dispatching), so its read
	 * deadline (task 031, armed at accept) is spent. Later requests on the
	 * connection are bounded by fpm_http_client_request_done() (issue #593). */
	if (gw->read_timeout_ms > 0) {
		fpm_http_read_deadline_disarm(gw, evcon ? evhttp_connection_get_bufferevent(evcon) : NULL);
	}

	if (gw->acl && !fpm_http_acl_check(gw->acl, peer_addr)) {
		/* ACL is about the direct network peer, so it (and its log entry) is
		 * deliberately NOT run through X-Forwarded-For -- an address rejected
		 * here is exactly the one that made the TCP connection. */
		fpm_http_log_response(gw, req, peer_addr, NULL, 403, 0, NULL);
		evhttp_send_error(req, 403, "Forbidden");
		fpm_http_count_local(gw);	/* #390: answered here, not forwarded */
		return;
	}

	/* An absolute-form authority too long for the FPM_HTTP_AUTHORITY_MAX buffer would be
	 * silently replaced by the Host header in some places and not in others
	 * (#534): refuse it up front. */
	{
		char authority[FPM_HTTP_AUTHORITY_MAX];

		if (fpm_http_absolute_authority(evhttp_request_get_uri(req), authority, sizeof(authority)) < 0) {
			fpm_http_log_response(gw, req, peer_addr, NULL, 400, 0, NULL);
			evhttp_send_error(req, HTTP_BADREQUEST, "Bad Request");
			fpm_http_count_local(gw);
			return;
		}
	}

	/* Resolved once per request: whether the direct peer is a trusted proxy
	 * (http.trusted_proxies) and, if so, what X-Forwarded-For/-Proto/-Port say.
	 * See fpm_http_forwarded.h. Everything downstream -- CGI vars and the
	 * access log -- uses this single decision. */
	fpm_http_forwarded_resolve(gw->trusted_proxies_acl, peer_addr,
		evhttp_request_get_input_headers(req), &fwd);
#ifdef HAVE_FPM_HTTP_TLS
	/* This specific connection terminated TLS right here in the gateway
	 * (gw->tls_ctx != NULL, see fpm_http_gateway_run()), which is a stronger
	 * signal than any X-Forwarded-Proto a trusted proxy might have sent --
	 * override to "https" regardless of what fpm_http_forwarded_resolve()
	 * concluded. fpm_http_build_request() only ever reads fwd.scheme/https,
	 * so this is the one place that needs to know about TLS at all. */
	if (gw->tls_ctx) {
		fwd.scheme = "https";
		fwd.https = 1;
	}
#endif
	effective_addr = fwd.remote_addr[0] ? fwd.remote_addr : peer_addr;

	/* ping.path first (issue #382): the probe proves THIS process is alive, so
	 * it is answered before anything else, even a path that also sits under an
	 * operator base. */
	if (fpm_http_serve_ping(gw, req, effective_addr)) {
		fpm_http_count_ping(gw);	/* #390: local answer, and its own ping count */
		return;
	}

	/* Issue #389: the operator namespace next, after ping and before the
	 * static lookup and routing, so a route can never shadow <base>/<pool>
	 * and a miss is a local 404. */
	if (fpm_http_operator_request(gw, req, peer_addr, effective_addr, &fwd, peer_port, client)) {
		return;
	}

	/* Local responses next: there is no point building FastCGI parameters or
	 * occupying a worker slot for a file we will serve ourselves. -1 = "not
	 * checked" (not GET/HEAD, or http.static = 0): fpm_http_build_request() then
	 * decides whether it needs its own stat() for http.front_controller. */
	{
		int script_missing = -1;

		if (fpm_http_try_local(gw, req, effective_addr, &script_missing)) {
			fpm_http_count_local(gw);	/* #390: static file or ACME challenge */
			return;
		}

		c = calloc(1, sizeof(*c));
		c->gw = gw;
		c->req = req;
		c->evcon = evcon;
		c->client = client;		/* #390 */
		c->status = -1;
		c->queue_wait_ms = -1;		/* "never waited"; http.pool_full_policy = wait may still set it, issue #309 */
		c->fwd = fwd;
		c->peer_port = peer_port;
		if (peer_addr) {
			strlcpy(c->peer_addr, peer_addr, sizeof(c->peer_addr));
		}

		/* Routing BEFORE serialization, and this order is a requirement, not a
		 * convenience (issue #340): the bytes written below are in the target's
		 * wire protocol, so the target has to be known first. With one
		 * transport the order costs nothing; without it, adding the second one
		 * (#344) would have to unpick FastCGI framing from the request path. */
		c->target = fpm_http_route(gw, req);
		/* Issue #388: on the gateway a request matching no http.route[]
		 * prefix is answered here, locally, and is never forwarded anywhere.
		 * 404 is the gateway's existing "nothing here" (a static miss already
		 * answers it); 502/503 would mean a target exists and failed or is
		 * full, which is not this case. The access log's target field is "-"
		 * (the NULL passed below), which is what #341's field already means
		 * for a request this gateway answered itself. */
		if (!c->target) {
			fpm_http_log_response(gw, req, effective_addr, NULL, HTTP_NOTFOUND, 0, NULL);
			evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
			fpm_http_count_local(gw);	/* #390: no route covers this path */
			c->evcon = NULL;
			fpm_http_conn_free(c);
			return;
		}
		/* Issue #341: fpmng_gateway_requests_total{target=...} -- every request
		 * this gateway routed to a target, counted here regardless of how it is
		 * eventually answered (200, a proxied error, a 503 from the reject path
		 * below). What upstreams_used/_max describe is pressure on the target;
		 * this is the traffic that pressure is a rate OF. */
		fpm_http_counter_incr(c->target->requests_total);
		fpm_http_dispatch(gw, c, script_missing);
	}
}


#else /* HAVE_FPM_HTTP */

int fpm_http_init_pool(struct fpm_worker_pool_s *wp)
{
	(void)wp;
	return 0;
}

int fpm_http_init_pool_with_capacity(struct fpm_worker_pool_s *wp, unsigned capacity)
{
	(void)wp;
	(void)capacity;
	return 0;
}

int fpm_http_validate_pool(struct fpm_worker_pool_s *wp)
{
	(void)wp;
	return 0;
}

void fpm_http_render_metrics_prometheus(struct fpm_worker_pool_s *wp, struct fpm_operator_buf_s *b)
{
	(void)wp;
	(void)b;
}

/* Issue #390: the gateway type does not exist in a build without the HTTP
 * gateway, so neither does its counters segment -- both report nothing rather
 * than failing to link. */
unsigned long fpm_http_gateway_baseline_requests(struct fpm_worker_pool_s *wp)
{
	(void)wp;
	return 0;
}

void fpm_http_gateway_operator_status(struct fpm_worker_pool_s *wp, const char *query,
	struct fpm_operator_reply_s *reply)
{
	(void)wp;
	(void)query;
	(void)reply;
}

#endif
