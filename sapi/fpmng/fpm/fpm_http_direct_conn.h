/* Per-connection accounting for a direct worker (issue #61).
 *
 * Two things this file is for, both of which a direct pool had no answer to
 * before:
 *
 * 1. AN ABSOLUTE DEADLINE FOR THE FIRST REQUEST. `http.read_timeout` is
 *    documented in fpm_conf.h as "one budget for the whole client-side read
 *    (headers + body)", and on the http gateway it is exactly that. On a
 *    direct pool it was only evhttp_set_timeout_tv(), which is a bufferevent
 *    idle timeout: every byte that arrives resets it. Measured on the poligon
 *    2026-09-11 against a pool with http.read_timeout = 3000: a client sending
 *    one byte of the request line every 2 s held the connection for 56 s and
 *    was then served normally. A client that sends nothing at all is dropped
 *    at 3.0 s, so only the trickle was unbounded -- which is the shape of
 *    slowloris. The deadline armed here makes the directive mean on a direct
 *    pool what it already means on the gateway.
 *
 * 2. CONNECTION LIMITS. `http.max_connections` and
 *    `http.max_connections_per_client`, both off by default. A worker at its
 *    limit stops accepting instead of refusing: the listening socket is shared
 *    by every child of the pool, so a connection left in the queue is one a
 *    sibling can take, and a connection refused with a response is one nobody
 *    can.
 *
 * The mechanism is the gateway's (struct fpm_http_read_deadline_s in
 * fpm_http.c), for the reason issue #90 established there: libevent 2.1.12
 * offers evhttp_set_bevcb() as the only per-accepted-connection hook, it runs
 * before the evhttp_connection exists, and evhttp frees the bufferevent
 * whenever the connection ends -- routinely before any timer of ours fires.
 * Holding a bufferevent reference is what makes the pointer safe to keep;
 * without it, a deadline firing on a dropped connection is a use-after-free.
 *
 * Consequences of that reference, both bounded and both deliberate:
 *
 * - A connection evhttp has finished with keeps its fd until this file
 *   notices and drops the reference. For a connection still reading its first
 *   request the EOF watcher notices immediately; for one that is only being
 *   counted, the worker's 10 ms tick sweep does, so an fd can outlive its
 *   connection by one tick.
 * - The sweep is a pointer read per tracked connection, 100 times a second.
 *   http.max_connections is what bounds the number of tracked connections --
 *   which is why validation requires it whenever
 *   http.max_connections_per_client is set: the per-client count is a walk of
 *   this list, and an unbounded list would make the accept path grow with the
 *   flood it is supposed to survive. Where no total cap is configured the walk
 *   happens on the tick only, never per accept, so accepting stays O(1)
 *   whatever the list length.
 *
 * Since issue #64 a node outlives the first request and is kept for the whole
 * life of the connection even when no limit is configured, because the live
 * count is what the status page reports. Before that a deadline-only worker
 * dropped the node the moment its request arrived.
 *
 * What is NOT here, and is rejected by validation rather than ignored:
 * per-connection limits for later requests on a keep-alive connection (the
 * deadline covers the first one; libevent offers no request-start hook), and
 * any limit shared between the children of a pool -- each worker counts its
 * own connections, which is what "per worker" in the documentation means.
 */

#ifndef FPM_HTTP_DIRECT_CONN_H
#define FPM_HTTP_DIRECT_CONN_H 1

struct event_base;
struct bufferevent;
struct fpm_http_direct_conns;

struct fpm_http_direct_conns_limits {
	const char *pool;	/* for log messages */
	int read_timeout_ms;	/* first-request deadline; 0 disables it */
	int max_connections;	/* per worker; 0 = unlimited */
	int max_per_client;	/* per peer address, per worker; 0 = unlimited */
};

/* NULL only on OOM. A worker whose limits are all off still gets an object:
 * the deadline alone is worth tracking, and the caller has one less branch. */
struct fpm_http_direct_conns *fpm_http_direct_conns_new(struct event_base *base,
	const struct fpm_http_direct_conns_limits *limits);
void fpm_http_direct_conns_free(struct fpm_http_direct_conns *conns);

/* From the bevcb, with the bufferevent it is about to return. The fd and the
 * peer are not known yet -- evhttp calls bufferevent_setfd() right after --
 * so everything that needs them happens one loop pass later. */
void fpm_http_direct_conns_accepted(struct fpm_http_direct_conns *conns, struct bufferevent *bev);

/* The first request on this connection has fully arrived: its deadline is
 * spent. Safe to call for a connection that is not tracked, and for every
 * request rather than only the first.
 *
 * Returns -1 when this connection is over http.max_connections_per_client. The
 * node is gone by then, but the connection is NOT closed: the caller is inside
 * evhttp's request callback and is expected to answer 503 and let
 * evhttp_send_error()'s Connection: close end it. This file does not force the
 * close itself there -- see fpm_direct_conn_check_peer() for why forcing it
 * would put a 1 us timeout under the caller's own response.
 *
 * The cap is enforced here, at the last moment before the answer, because the
 * alternative (the pickup pass, which is a zero-delay timer) races the
 * connection's own read callback and sometimes loses. */
int fpm_http_direct_conns_request(struct fpm_http_direct_conns *conns, struct bufferevent *bev);

/* False while this worker is at http.max_connections. The caller keeps its
 * listener disabled for as long as this says so. Not const: it sweeps first,
 * because a count that lags by a tick is a count that refuses connections on
 * behalf of connections that have already ended. */
int fpm_http_direct_conns_may_accept(struct fpm_http_direct_conns *conns);

/* Releases connections evhttp has finished with. Call from the worker's tick;
 * a no-op when no limit is configured. */
void fpm_http_direct_conns_sweep(struct fpm_http_direct_conns *conns);

/* What this file knows, for the status page (issue #64). All three are this
 * worker's own: live is a gauge that lags its connection by at most one sweep,
 * the other two are totals since the child started. The worker publishes them
 * into its shared slot from the same tick that sweeps, rather than this file
 * reaching into the scoreboard -- accounting belongs to whoever already owns a
 * slot. */
unsigned fpm_http_direct_conns_live(const struct fpm_http_direct_conns *conns);
/* Connections dropped because the first request did not arrive in time. */
unsigned long fpm_http_direct_conns_timed_out(const struct fpm_http_direct_conns *conns);
/* Connections refused by http.max_connections_per_client, in either shape. */
unsigned long fpm_http_direct_conns_refused(const struct fpm_http_direct_conns *conns);

#endif
