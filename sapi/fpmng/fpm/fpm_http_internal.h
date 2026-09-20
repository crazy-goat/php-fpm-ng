/* Internal surface shared between fpm_http.c (the gateway and its FastCGI
 * transport) and fpm_http_client.c (the HTTP/1.1 client transport, issue
 * #344). Everything here is private to the gateway implementation -- nothing
 * outside these two files includes this header. The struct definitions and
 * the TAILQ compatibility block moved out of fpm_http.c verbatim when the
 * second transport arrived; the comments below are theirs. */
#ifndef FPM_HTTP_INTERNAL_H
#define FPM_HTTP_INTERNAL_H

#include "fpm_config.h"

#ifdef HAVE_FPM_HTTP

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
#include "zend_smart_str.h"

#include "fpm_atomic.h"
#include "fpm_http_acl.h"
#include "fpm_http_forwarded.h"
#include "fpm_http_auth.h"
#include "fpm_http_access_log.h"
#include "fpm_tls_http.h"
#include "fpm_tls_reload.h"

#define FPM_HTTP_MAX_CGI_HEADERS (64 * 1024)
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
#define FPM_HTTP_BAD_GATEWAY     502 /* libevent has no constant for it */
#define FPM_HTTP_SERVICE_UNAVAIL 503 /* libevent has no constant for it */
#define FPM_HTTP_RETRY_AFTER     "1" /* Retry-After seconds sent with a 503 on a full pool */

typedef struct _fpm_http_conn fpm_http_conn;
typedef struct _fpm_http_upstream fpm_http_upstream;

struct fpm_http_gateway_s;

/* one gateway slot: which gw it belongs to, its index, and its crash-loop
 * bookkeeping (see fpm_http_gateway_on_exit()). Passed as the `arg` to
 * fpm_children_extra_watch() because fpm_children_extra.h only knows a bare
 * void* — this is where "which gateway, which slot" gets recovered. */
struct fpm_http_gw_slot_s {
	struct fpm_http_gateway_s *gw;
	unsigned index;
	struct {
		time_t window_start;
		unsigned count;
		int gave_up;
	} respawn;
	/* This process's error_log follow channel, created before its fork and
	 * replaced with the process (issue #134, fpm_error_log_follow.h). NULL
	 * when there is no file error_log to follow. */
	struct fpm_error_log_follow_s *log_follow;
};

/* ------------------------------------------------------------------------ *
 * Routing targets (issue #340)
 *
 * A gateway proxies to one or more backend pools, chosen per request by the
 * longest matching path prefix. Everything that used to be singular on the
 * gateway -- the upstream address, the persistent-connection list, the shared
 * budget and the queue of requests waiting for a free connection -- is per
 * TARGET, because the workers that enforce it are one set per target pool.
 * Two prefixes routed to the same pool therefore share one budget and one
 * queue; a prefix is a routing key, a target is a pool.
 *
 * Issue #388 removed the old weld's implicit target 0: pool.type = gateway
 * routes by http.route[] alone, at least one route is required at startup, and
 * a request matching none is a local 404. A row for "/" is an ordinary row like
 * any other, not a fallback the table always gains.
 * ------------------------------------------------------------------------ */

struct fpm_http_target_s;

/* Which wire protocol the gateway speaks to a target. Only FastCGI is
 * implemented here; HTTP/1.1 towards a pool.type = http-direct target is #344,
 * and the vtable below exists so that it is a new set of four functions rather
 * than a second request path. */
enum fpm_http_transport_e {
	FPM_HTTP_TARGET_FASTCGI = 0,
	/* Issue #344: plain HTTP/1.1 towards a pool.type = http-direct target,
	 * implemented in fpm_http_client.c behind the same vtable. */
	FPM_HTTP_TARGET_HTTP
};

/* Everything transport-specific about talking to a target, bound once at
 * config time from the target's pool.type. The four members are the four
 * points where this file used to name FastCGI directly.
 *
 * What is NOT here is deliberate: handing c->out to an upstream, the pending
 * buffer, the budget and the queue are bytes and bookkeeping, identical for
 * any stream protocol. Only who PRODUCES those bytes (write_request) and who
 * INTERPRETS the answer (on_readable) differ. */
struct fpm_http_transport_s {
	/* Opens one persistent connection to the target, budget included, or
	 * returns NULL when the target is full or the connect failed. */
	fpm_http_upstream *(*connect)(struct fpm_http_target_s *t);
	/* Serializes the request into c->out, ready to be handed to any
	 * connection of this target. Returns 0, or an HTTP status to answer the
	 * client with instead. Called from fpm_http_request() AFTER c->target is
	 * set -- a request may not be serialized before it is known who it is
	 * for. */
	int (*write_request)(fpm_http_conn *c, int script_missing_hint);
	/* The upstream socket's read callback, i.e. this protocol's parser. */
	void (*on_readable)(evutil_socket_t fd, short what, void *arg);
	/* Tears one connection down, returning its budget slot. */
	void (*drop)(fpm_http_upstream *up);
};

/* One backend pool this gateway may send requests to. */
struct fpm_http_target_s {
	struct fpm_http_gateway_s *gw;
	char *pool;				/* the target pool's name, for the log */
	char *listen_address;			/* where that pool takes requests */
	enum fpm_http_transport_e transport;
	const struct fpm_http_transport_s *ops;

	/* how many persistent connections all the gateways of this pool may hold
	 * to THIS target together; sized from the target pool's own
	 * pm.max_children, the number its workers actually enforce */
	unsigned max_upstreams;
	atomic_t *upstreams_used;		/* shared between the gateway processes */

	/* Issue #341: per-target counters, shared memory for the same reason
	 * upstreams_used is -- fpm_operator_pages.c's
	 * fpm_pool_type_s.render_metrics_prometheus hook runs in the operator
	 * endpoint's OWN child, a fork() taken after fpm_http_routes_build() has
	 * already allocated these, so that child's copy of `gw->targets` points
	 * at the same shared segment every gateway process updates. Never NULL
	 * once fpm_http_target_init() has returned 0 -- checked defensively at
	 * the two read sites anyway, the same caution upstreams_used itself
	 * takes at fpm_operator_pages.c. */
	atomic_t *requests_total;		/* requests routed to this target, whatever they answered */
	atomic_t *rejected_total;		/* of those, how many found no budget and got 503 */

	/* gateway process only */
	struct sockaddr_storage upstream_addr;
	socklen_t upstream_len;
	TAILQ_HEAD(, _fpm_http_upstream) upstreams;
	unsigned nupstreams;
	TAILQ_HEAD(, _fpm_http_conn) waiting;	/* requests without a free connection yet */
};

/* One row of the routing table: a path prefix and the target it selects. The
 * rows are sorted longest prefix first at config time, so the lookup is the
 * first match. */
struct fpm_http_route_s {
	char *prefix;
	size_t prefix_len;
	struct fpm_http_target_s *target;
};

/* one gateway family per pool */
struct fpm_http_gateway_s {
	struct fpm_http_gateway_s *next;
	char *pool;
	char *listen_address;			/* where the gateway listens when pool.type = gateway (the public port)
						 * or, for the retired weld's routing, where the pool takes FastCGI */
	/* Issue #388: copied from fpm_pool_type_s.proxy_only at settings time, so
	 * every branch below asks this flag instead of re-resolving the type. A
	 * proxy_only gateway's `listen` is its PUBLIC port (the child accepts on
	 * the master's listening_socket directly), its routing table is exactly
	 * http.route[] with no implicit own-pool row, and its process count is
	 * http.gateways alone. */
	int proxy_only;
	char *docroot;
	int listen_fd;
	int plain_listen_fd;
	int backlog;
	int reuseport;					/* every gateway binds its own SO_REUSEPORT socket (http.reuseport) */
	unsigned nproc;
	pid_t *pids;
	struct fpm_http_gw_slot_s **slots;		/* one per pids[i], see fpm_http_gw_slot_s */
	int static_files;				/* http.static, per pool: fork() copies it into every gateway process */
	int idle_ms;					/* http.idle_timeout, milliseconds; 0 = never drop an idle pinned connection */
	struct timeval idle_timeout;			/* idle_ms split into {sec, usec} for event_add() */
	int read_timeout_ms;				/* http.read_timeout, milliseconds; 0 = no client-side read deadline */
	struct timeval read_timeout;			/* read_timeout_ms split into {sec, usec} for evhttp_set_timeout_tv() */
	/* http.pool_full_policy, issue #309. wait_policy is FPM_HTTP_POOL_FULL_REJECT
	 * (the default, unchanged behavior: fpm_http_pump_once() drains gw->waiting
	 * to a 503 the instant the budget is exhausted) or FPM_HTTP_POOL_FULL_WAIT,
	 * gated per pool to workloads the operator has judged IO-light -- see
	 * docs/http-gateway-pool-full.md. wait_queue_max and wait_ms are read only
	 * when wait_policy is on; fpm_http_validate_pool() has already refused a
	 * wait policy with either bound at zero. */
	int wait_policy;
	int wait_queue_max;
	int wait_ms;
	struct timeval wait_bound;			/* wait_ms split into {sec, usec} for evtimer_add() */
	size_t max_body;					/* http.max_body, bytes; evhttp buffers a whole body in memory before dispatch (task 031) */
	char *allowed_clients;				/* http.allowed_clients, raw string kept for fpm_http_acl_parse() */
	struct fpm_http_acl_s *acl;			/* NULL = no restriction, see fpm_http_acl.h */
	char *http_listen_override;			/* http.listen; NULL = derive from listen_address (port + 1) */
	char *plain_listen_address;			/* http.plain_listen; redirect-only companion, NULL = disabled */
	char *trusted_proxies;				/* http.trusted_proxies, raw string kept for fpm_http_acl_parse() */
	struct fpm_http_acl_s *trusted_proxies_acl;	/* NULL = trust nobody, see fpm_http_forwarded.h */
	char *access_log_path;				/* http.access_log; NULL = disabled */
	struct fpm_http_access_log_s *access_log;	/* gateway process only, NULL in the master */
	char *front_controller;			/* http.front_controller; empty = fallback disabled (today's behavior) */
	int front_controller_ok;			/* validated once by the master, before the first fork -- see fpm_http_front_controller_validate() */

	/* ping.path/ping.response, answered locally -- issue #382. NULL ping_path
	 * means the directive is unset, exactly as fpm_conf.c leaves it; a set
	 * ping_path always has a non-NULL ping_response by the time fpm_conf.c is
	 * done validating (it defaults to "pong"), copied again here so a gateway
	 * process depends on nothing beyond its own fork()ed memory. */
	char *ping_path;
	char *ping_response;

	/* access.suppress_path[], copied the same way -- fpm_http_log_response()
	 * checks every entry before writing a line. First real consumer of the
	 * directive on this listener (issue #382); see
	 * fpm_http_direct_access_log.c for the pool.type = http-direct twin. */
	char **suppress_paths;
	unsigned suppress_paths_count;

	/* Pool's resolved 'user'/'group' (wp->set_uid/set_gid/set_user, copied
	 * once in the master by fpm_http_gateway_settings() -- fpm_unix_conf_wp()
	 * has already resolved them by then, see fpm_http.c:fpm_http_gateway_drop_privileges).
	 * uid 0 means the pool declared none (only possible under FPM's explicit
	 * run_as_root escape hatch): the gateway then keeps the master's identity,
	 * same as fpm_unix_init_child() does for workers. */
	uid_t drop_uid;
	gid_t drop_gid;
	char *drop_user;

#ifdef HAVE_FPM_HTTP_TLS
	/* http.tls_cert/http.tls_key; NULL = plain HTTP, exactly as today.
	 * gw->tls is loaded INTO MEMORY in the master, BEFORE the first child forks
	 * (fpm_tls_http_load()) — fork() copies it. gw->tls_ctx is per-process: each
	 * child builds its OWN SSL_CTX from bytes the master read, so the shared
	 * ticket key supports session resumption across processes; see
	 * fpm_tls_http.h. Which bytes: the generation gw->reload currently
	 * publishes when there is one, otherwise gw->tls — see
	 * fpm_http_gateway_run() and issue #91, a gateway respawned after a
	 * reload must not start on gw->tls's startup certificate. */
	struct fpm_tls_http_s *tls;			/* NULL in the child after a failed startup */
	SSL_CTX *tls_ctx;				/* only in the child, NULL in the master */
	/* NULL only when shared-memory allocation failed or the startup pair is
	 * already bigger than the reload buffer (fpm_tls_reload_master_init());
	 * the gateway then behaves exactly as it did before task 040.
	 * http.tls_reload_check = 0 does NOT make this NULL — the struct is still
	 * built, it just never arms a timer, so generation 0 stays published
	 * forever and every child snapshots the startup bytes. */
	struct fpm_tls_reload_s *reload;
#endif

	/* Both of these are plain ints and both are read from code that is NOT
	 * inside #ifdef HAVE_FPM_HTTP_TLS -- fpm_http_plain_request(), the
	 * listener bind, fpm_http_gateway_open_tls_listener() -- so they live
	 * outside it. A build without libevent_openssl (sapi/fpmng/config.m4)
	 * simply never leaves the defaults below.
	 *
	 * http.tls_wait_for_cert, issue #172: this pool is allowed to start
	 * before its certificate exists. Cleared by fpm_http_gateway_settings()
	 * once it is established that the certificate is in fact already there,
	 * or that nothing could ever open the listener -- after that point the
	 * ordinary fail-closed behaviour applies unchanged. */
	int tls_wait_for_cert;
	/* Child only: has THIS gateway process opened its TLS listener yet?
	 * 0 is NO_CERT, 1 is READY. Defaults to 1, i.e. the pre-#172 behaviour,
	 * and is only ever lowered inside the opt-in branch.
	 *
	 * Derived from gw->tls_ctx and from nothing else -- see the comment where
	 * it is assigned. Sampling the shared reload state separately from the
	 * context build would let the two disagree when the master publishes a
	 * generation between the two reads, and one of those disagreements puts
	 * a listening socket in front of a NULL SSL_CTX, which fpm_http_bevcb()
	 * serves as cleartext.
	 *
	 * One-way by construction -- nothing ever sets it back to 0, which is
	 * issue #172 criterion 5: a certificate file deleted under a serving pool
	 * must not take TLS down. The operator sees the deletion as the master's
	 * ordinary "skip this tick" silence plus an expiring certificate, not as
	 * an outage we caused. */
	int tls_ready;

	/* Routing (issue #340), built once in the master before the first gateway
	 * forks and never touched again -- a reload restarts the gateway, there is
	 * no hot reload of routes. targets[0] is the pool's own listener whenever
	 * http.route[] did not claim "/" itself. nroutes >= ntargets: one row per
	 * prefix, several rows may point at one target. */
	struct fpm_http_target_s *targets;
	unsigned ntargets;
	struct fpm_http_route_s *routes;
	unsigned nroutes;
	/* Issue #341: true when THIS pool set http.route[] at all (even if every
	 * entry claims "/" and leaves ntargets == 1). Gates the access log's
	 * target field: a pool that never opted into routing must log "-" on
	 * every line, byte-for-byte what it logged before this issue, and this is
	 * the one flag that tells fpm_http_log_response() so without it having to
	 * re-derive "did this pool configure routing" from the shape of the
	 * table it already built. */
	int has_routes;

	/* gateway process only */
	struct event_base *base;
	struct evhttp *http;
	/* http.fault_upstream_write, see fpm_http_upstream_write_must_fail().
	 * 0 = off, which is the value every real deployment has. The counter is
	 * per gateway process: fork() copies a zero into each one. */
	int fault_write_at;
	int fault_writes;
	/* fpm_http_pump() is on the stack. Handing a request to an upstream can
	 * fail synchronously, and the failure path ends in fpm_http_pump() again
	 * (fpm_http_upstream_fail()); with one queued request per failure that
	 * recursion is as deep as gw->waiting is long. A nested call therefore
	 * asks the running one for another round instead of dispatching itself,
	 * which also keeps "who may free a connection" answerable: only the
	 * outermost loop walks gw->waiting. Issue #129. */
	int pumping;
	int pump_again;
	struct fpm_http_read_deadline_s *deadlines;	/* armed read deadlines, one per connection still reading its first request */
};

/* One armed read deadline per accepted connection (task 031). Bounds the total
 * time a client may spend delivering ONE request, regardless of how the bytes
 * are spaced: libevent's own evhttp timeout (bufferevent read timeout) is an
 * *idle* timer restarted on every received byte, so a slow-loris client
 * trickling one byte at a time never trips it. The deadline is armed in the
 * gateway's bevcb and disarmed by fpm_http_request() -- evhttp invokes the
 * request callback only after the whole request (headers + body) has arrived,
 * so reaching it means the client delivered in time. A deadline that fires
 * frees the bufferevent, closing the connection mid-read with no response:
 * there is no complete request to answer to.
 *
 * Keep-alive: the deadline covers the first request on a connection. Later
 * requests on the same connection are a deliberate gap (arming a new one
 * would need a request-start hook libevent does not offer); the per-read
 * idle timeout still applies to them.
 *
 * The gw->deadlines list exists only so fpm_http_request() can find and
 * disarm its own deadline by bufferevent pointer: one linear scan per
 * dispatched request, over one node per connection still reading its first
 * request, so n stays small. */
struct fpm_http_read_deadline_s {
	struct fpm_http_gateway_s *gw;
	struct bufferevent *bev;		/* the connection's bufferevent; one reference of ours is held for the node's whole lifetime, see arm() */
	evutil_socket_t fd;			/* the connection's fd, for the EOF watcher; -1 until known */
	struct event *ev;			/* the one-shot deadline timer */
	struct event *ev_eof;			/* first a zero timer (fd pickup), then the persistent EOF watcher */
	struct fpm_http_read_deadline_s *next;
};


/* one HTTP request being proxied */
struct _fpm_http_conn {
	struct fpm_http_gateway_s *gw;
	/* Which backend pool this request is for (issue #340). Set by
	 * fpm_http_route() BEFORE one byte of the request is serialized, and
	 * everything queue-, budget- and transport-related reads it from here
	 * afterwards: c->target->waiting is the queue this request joins, and
	 * c->target->ops is the protocol its bytes were written in. Never NULL
	 * from fpm_http_request()'s serialization step onwards -- the table
	 * always has a row that matches, because a table with no "/" row of its
	 * own gets the gateway's own listener as one. */
	struct fpm_http_target_s *target;
	struct evhttp_request *req;
	struct evhttp_connection *evcon;
	fpm_http_upstream *upstream;		/* while in flight */
	/* 1 exactly while this connection is linked into gw->waiting, so
	 * fpm_http_conn_free() knows whether it still has to unlink it.
	 *
	 * Three sites, and all three are needed -- the flag is not dead weight:
	 *   - set at the only insert, TAILQ_INSERT_TAIL in fpm_http_request();
	 *   - cleared at the two removals that keep the connection alive
	 *     afterwards: the dispatch in fpm_http_pump() and its 503 drain loop;
	 *   - read (not cleared -- it frees c) by the third removal, the one in
	 *     fpm_http_conn_free() itself. That is the path a client takes when it
	 *     disconnects while still queued: fpm_http_client_closed() ->
	 *     fpm_http_conn_free() with queued == 1. It is the reason the flag
	 *     exists, and it is why clearing the flag at the two explicit removals
	 *     does not make it removable.
	 *
	 * Nothing else touches the list or the flag -- keep it that way, or
	 * fpm_http_conn_free() either double-removes or leaks a dangling list
	 * entry (issue #107). */
	int queued;
	TAILQ_ENTRY(_fpm_http_conn) link;

	smart_str params;					/* FCGI_PARAMS payload being assembled */
	smart_str out;						/* records ready to go upstream */
	int params_oversize;				/* one name/value pair did not fit a record -- see fpm_http_param() */

	smart_str cgi_headers;				/* CGI header block until it is complete */
	int headers_sent;

	char peer_addr[FPM_HTTP_FORWARDED_ADDR_LEN];		/* direct TCP peer, before X-Forwarded-For */
	ev_uint16_t peer_port;
	struct fpm_http_forwarded_result_s fwd;		/* resolved once in fpm_http_request() */
	char remote_addr[FPM_HTTP_FORWARDED_ADDR_LEN];		/* effective REMOTE_ADDR: fwd.remote_addr or peer_addr */
	char remote_user[FPM_HTTP_AUTH_USER_LEN];		/* from Authorization: Basic, for CGI var and access log */
	int status;						/* HTTP status finally sent, -1 until known; for the access log */
	size_t bytes_out;					/* body bytes sent to the client, for the access log */

	/* http.pool_full_policy = wait (issue #309). All three are dead unless
	 * the pool opted in: when this request was put on gw->waiting, the
	 * one-shot timer that bounds how long it may stay there (freed the
	 * moment it leaves the queue, by whichever path -- dispatch, an expired
	 * wait, or a client that disconnects while still queued), and how long
	 * it actually waited (-1 until dispatched, matching how fpm_http_pump()
	 * reports queue_wait_ms in the access log). */
	struct timeval wait_since;
	struct event *wait_timer;
	long queue_wait_ms;
};

/* One persistent FastCGI connection to the pool, serving one request at a time.
 * Plain socket rather than a bufferevent: the read event is registered once and
 * never touched, and writes go straight out, which keeps epoll_ctl and the
 * FIONREAD ioctl that evbuffer_read does out of the hot path. */
struct _fpm_http_upstream {
	struct fpm_http_gateway_s *gw;
	/* The target this connection belongs to (issue #340): whose budget it
	 * holds, whose list it is on, whose queue it serves. Kept alongside `gw`
	 * rather than instead of it because most of this file's uses of `gw` are
	 * about the GATEWAY process (its event base, its log, its timeouts), not
	 * about the backend. t->gw is always this gw. */
	struct fpm_http_target_s *t;
	int fd;
	struct event *ev_read;
	struct event *ev_write;				/* only pending while a write did not fit or we are connecting */
	int connecting;
	smart_str pending;					/* not yet written to the pool */
	size_t pending_off;
	int busy;							/* a request is in flight, even if its client is gone */
	fpm_http_conn *current;				/* NULL when idle or when the client went away */
	TAILQ_ENTRY(_fpm_http_upstream) link;

	/* `active` is non-zero while a caller still dereferences this upstream
	 * after a callback may have decided to free it -- today only
	 * fpm_http_upstream_data()'s record loop, which can reach
	 * fpm_http_request_done() -> fpm_http_pump() -> a synchronous write
	 * failure on this very connection. fpm_http_upstream_drop() then detaches
	 * the upstream, sets `dead` and returns; the caller does the free() on its
	 * way out. Issue #129. */
	int active;
	int dead;

	/* FastCGI record stream from the pool */
	unsigned char rec_hdr[8];
	int rec_hdr_len, rec_type, rec_len, rec_pad;

	/* A child that died and a child that refused the request head are the same
	 * event on this socket: the connection goes away and no reply arrives. The
	 * two facts that do separate them are known here and were not kept
	 * anywhere (issue #118): whether the request went out complete, and
	 * whether one byte ever came back. See fpm_http_upstream_fail(). */
	int req_written;					/* the whole request reached the socket */
	int reply_seen;						/* at least one byte arrived from the pool for `current` */
	struct timeval req_written_at;		/* loop time when the request was fully written */

	/* Issue #344: the HTTP/1.1 transport's parser state (framing, body
	 * counters), owned and freed by fpm_http_client.c. NULL for a FastCGI
	 * connection -- this struct's rec_* fields above are already
	 * transport-specific by the same convention, just declared inline
	 * because FastCGI came first. */
	void *http;
};

/* ---------------------------------------------------------------- shared with fpm_http_client.c (issue #344) */

/* EAGAIN and EWOULDBLOCK are the same value on most systems, hence the macro dance */
static inline int fpm_http_would_block(int err)
{
	if (err == EAGAIN) {
		return 1;
	}
#if EWOULDBLOCK != EAGAIN
	if (err == EWOULDBLOCK) {
		return 1;
	}
#endif
	return 0;
}

int fpm_http_budget_take(struct fpm_http_target_s *t);
void fpm_http_budget_give_back(struct fpm_http_target_s *t);
void fpm_http_upstream_fail(fpm_http_upstream *up, int clean_eof);
void fpm_http_pump(struct fpm_http_gateway_s *gw);
void fpm_http_request_done(fpm_http_upstream *up);
void fpm_http_upstream_drop(fpm_http_upstream *up);
void fpm_http_upstream_free(fpm_http_upstream *up);
void fpm_http_upstream_write(fpm_http_upstream *up, const char *data, size_t len);
void fpm_http_upstream_flush(fpm_http_upstream *up);
void fpm_http_start_reply(fpm_http_conn *c, size_t head_len, size_t body_off);
void fpm_http_stdout(fpm_http_conn *c, const char *data, size_t len);
void fpm_http_finish(fpm_http_conn *c, int explained);
fpm_http_upstream *fpm_http_transport_connect(struct fpm_http_target_s *t);
const char *fpm_http_method_name(enum evhttp_cmd_type type);
/* Issue #344: the HTTP/1.1 client transport's vtable, defined in
 * fpm_http_client.c. A function, not an extern const, so the definition can
 * stay static to its file and the header stays linkage-free. */
const struct fpm_http_transport_s *fpm_http_target_http_ops(void);

#endif /* HAVE_FPM_HTTP */
#endif /* FPM_HTTP_INTERNAL_H */
