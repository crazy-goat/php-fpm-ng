/* Plain HTTP gateway for a pool (--with-fpm-http, needs libevent).
 *
 * For every TCP pool the master forks a few gateway processes that serve
 * plain HTTP on the FastCGI port + 1 with libevent's evhttp, sharing one
 * listening socket. A gateway behaves like a web server in front of the pool:
 * it talks FastCGI to the pool over a small set of persistent connections,
 * sends the request as FastCGI records and turns the FastCGI response back
 * into HTTP. Neither the FastCGI code nor the PHP workers know that HTTP
 * exists. Keep-alive, chunked request bodies, HEAD and request parsing are
 * evhttp's job. Without libevent the gateway is compiled out and FPM behaves
 * as before.
 *
 * Persistent connections: a kept FastCGI connection pins one PHP worker, so
 * the gateways together never hold more than pm.max_children of them and
 * requests beyond that wait in the gateway's own queue instead of the kernel
 * backlog. The budget is a counter in shared memory rather than a fixed share
 * per process, so a gateway that happens to get all the clients can still use
 * every worker. A worker waiting for the next request on a kept connection counts
 * as active, so dynamic spawns spare workers for everyone else as it should.
 * ondemand never reaps such a worker though, and with any pm a pinned worker
 * is unavailable to other FastCGI clients (nginx, the status page), so an
 * idle connection is dropped after FPM_HTTP_IDLE_MS (env override, 0 keeps
 * them forever).
 */

#include "fpm_config.h"

#include "fpm.h"
#include "fpm_http.h"

#ifdef HAVE_FPM_HTTP

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <grp.h>
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
#include "fpm_http_forwarded.h"
#include "fpm_http_auth.h"
#include "fpm_http_access_log.h"
/* For FPM_HTTP_HEADER_NAME_MAX and FPM_HTTP_HEADERS_MAX: the bound on a
 * request header name (issue #115) and on the whole request header block
 * (issue #117) is the same on both transports, so each has one definition. */
#include "fpm_http_direct_request.h"
#include "fpm_children_extra.h"
#include "fpm_http_tls.h"
#include "fpm_http_tls_reload.h"
#include "zlog.h"

#define FPM_HTTP_GATEWAYS_DEFAULT 2			/* http.gateways default; also the FPM_HTTP_GATEWAYS env fallback */
#define FPM_HTTP_IDLE_MS         500		/* http.idle_timeout default (ms); release a pinned worker after this much idle time */
#define FPM_HTTP_READ_TIMEOUT_MS 5000		/* http.read_timeout default (ms); one budget for reading the whole request (headers + body) */
/* A crash loop (bad bind, OOM, ...) must not turn into an unbounded fork()
 * storm: after this many respawns within RESPAWN_WINDOW seconds, a gateway
 * slot gives up and stays dead until the next reload. */
#define FPM_HTTP_RESPAWN_MAX_BURST 5
#define FPM_HTTP_RESPAWN_WINDOW_SEC 10
#define FPM_HTTP_MAX_BODY        (32 * 1024 * 1024)	/* http.max_body default; the gateway buffers a whole request body in memory (task 031) */
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
};

/* one gateway family per pool */
struct fpm_http_gateway_s {
	struct fpm_http_gateway_s *next;
	char *pool;
	char *listen_address;			/* where the pool takes FastCGI */
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
	 * (fpm_http_tls_load()) — fork() copies it. gw->tls_ctx is per-process: each
	 * child builds its OWN SSL_CTX from bytes the master read, so the shared
	 * ticket key supports session resumption across processes; see
	 * fpm_http_tls.h. Which bytes: the generation gw->reload currently
	 * publishes when there is one, otherwise gw->tls — see
	 * fpm_http_gateway_run() and issue #91, a gateway respawned after a
	 * reload must not start on gw->tls's startup certificate. */
	struct fpm_http_tls_s *tls;			/* NULL in the child after a failed startup */
	SSL_CTX *tls_ctx;				/* only in the child, NULL in the master */
	/* NULL only when shared-memory allocation failed or the startup pair is
	 * already bigger than the reload buffer (fpm_http_tls_reload_master_init());
	 * the gateway then behaves exactly as it did before task 040.
	 * http.tls_reload_check = 0 does NOT make this NULL — the struct is still
	 * built, it just never arms a timer, so generation 0 stays published
	 * forever and every child snapshots the startup bytes. */
	struct fpm_http_tls_reload_s *reload;
#endif

	/* how many persistent connections all the gateways of this pool may hold together */
	unsigned max_upstreams;
	atomic_t *upstreams_used;		/* shared between the gateway processes */

	/* gateway process only */
	struct event_base *base;
	struct evhttp *http;
	struct sockaddr_storage upstream_addr;
	socklen_t upstream_len;
	TAILQ_HEAD(, _fpm_http_upstream) upstreams;
	unsigned nupstreams;
	TAILQ_HEAD(, _fpm_http_conn) waiting;	/* requests without a free connection yet */
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

static struct fpm_http_gateway_s *gateways = NULL;

/* one HTTP request being proxied */
struct _fpm_http_conn {
	struct fpm_http_gateway_s *gw;
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
};

/* One persistent FastCGI connection to the pool, serving one request at a time.
 * Plain socket rather than a bufferevent: the read event is registered once and
 * never touched, and writes go straight out, which keeps epoll_ctl and the
 * FIONREAD ioctl that evbuffer_read does out of the hot path. */
struct _fpm_http_upstream {
	struct fpm_http_gateway_s *gw;
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
};

static void fpm_http_pump(struct fpm_http_gateway_s *gw);
static void fpm_http_read_deadline_disarm(struct fpm_http_gateway_s *gw, struct bufferevent *bev);
static void fpm_http_read_deadline_forget(struct fpm_http_read_deadline_s *dl);
static void fpm_http_read_deadline_eof(evutil_socket_t fd, short what, void *arg);
static void fpm_http_read_deadline_arm_eof(evutil_socket_t fd, short what, void *arg);

/* Claims one of the pool's workers for a persistent connection, or fails when they are all taken. */
static int fpm_http_budget_take(struct fpm_http_gateway_s *gw)
{
	while (1) {
		unsigned long used = *gw->upstreams_used;	/* atomic_t is an integer of some width on every branch of fpm_atomic.h */

		if (used >= gw->max_upstreams) {
			return 0;
		}
		if (atomic_cmp_set(gw->upstreams_used, used, used + 1)) {
			return 1;
		}
	}
}

static void fpm_http_budget_give_back(struct fpm_http_gateway_s *gw)
{
	while (1) {
		unsigned long used = *gw->upstreams_used;	/* atomic_t is an integer of some width on every branch of fpm_atomic.h */

		if (used == 0 || atomic_cmp_set(gw->upstreams_used, used, used - 1)) {
			return;
		}
	}
}

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
static int fpm_http_listen(const char *pool, const char *listen_address, const char *http_address, int backlog, int reuseport);

/* Local (server-side) address and port of one HTTP connection, for SERVER_ADDR/SERVER_PORT.
 * Unlike the pool's listen address (which may be a wildcard "*"), this is the real address
 * the client actually connected to -- correct even under SO_REUSEPORT or 0.0.0.0 binds.
 * Both out buffers are left empty ("") when nothing sensible can be reported (e.g. the
 * gateway's own HTTP listener is a unix socket, or the fd is not available). */
static void fpm_http_local_addr(struct evhttp_connection *evcon, char *addr_buf, size_t addr_size,
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

static const char *fpm_http_method_name(enum evhttp_cmd_type type);
static void fpm_http_front_controller_validate(struct fpm_http_gateway_s *gw);

/* Single choke point for the access log: pulls method/URI/protocol/Referer/User-Agent
 * straight from the evhttp_request, callers only supply what they already know
 * (effective remote_addr, remote_user if any, final status, body bytes sent). */
static void fpm_http_log_response(struct fpm_http_gateway_s *gw, struct evhttp_request *req,
		const char *remote_addr, const char *remote_user, int status, size_t bytes)
{
	if (!gw->access_log) {
		return;
	}
	fpm_http_access_log_write(gw->access_log, remote_addr, remote_user,
		fpm_http_method_name(evhttp_request_get_command(req)), evhttp_request_get_uri(req),
		req->major, req->minor, status, bytes,
		evhttp_find_header(evhttp_request_get_input_headers(req), "Referer"),
		evhttp_find_header(evhttp_request_get_input_headers(req), "User-Agent"));
}

/* ---------------------------------------------------------------- FastCGI encoding */

static void fpm_http_fcgi_record(smart_str *out, int type, const char *data, size_t len)
{
	unsigned char hdr[8] = {FCGI_VERSION_1, (unsigned char)type, 0, 1, (unsigned char)(len >> 8), (unsigned char)len, (unsigned char)((8 - len % 8) % 8), 0};
	static const char zeros[8] = {0};

	smart_str_appendl(out, (char*)hdr, sizeof(hdr));
	smart_str_appendl(out, data, len);
	smart_str_appendl(out, zeros, hdr[6]);
}

static void fpm_http_fcgi_len(smart_str *out, size_t len)
{
	if (len < 0x80) {
		smart_str_appendc(out, (char)len);
	} else {
		unsigned char b[4] = {(unsigned char)((len >> 24) | 0x80), (unsigned char)(len >> 16), (unsigned char)(len >> 8), (unsigned char)len};
		smart_str_appendl(out, (char*)b, 4);
	}
}

/* name/value pairs never straddle records, the receiver decodes each record on its own */
static void fpm_http_param(fpm_http_conn *c, const char *name, const char *value)
{
	size_t name_len = strlen(name), value_len = strlen(value);
	size_t pair_len = (name_len < 0x80 ? 1 : 4) + (value_len < 0x80 ? 1 : 4) + name_len + value_len;

	/* A pair may not straddle records (see above), so one that cannot fit an
	 * empty record cannot be sent at all: the request is refused rather than
	 * mangled. Without this the length silently wrapped in the record header
	 * -- fpm_http_fcgi_record() writes contentLength as two bytes, so 65539
	 * became 3 -- and the worker parsed request bytes as record headers.
	 * Reachable inside the 64 KiB block bound this commit sets, measured on
	 * 192.168.8.50, 2026-09-09: `GET / HTTP/1.0` plus one 65520-byte header
	 * line gives a 65533-byte pair, and a 65523-byte `.php` URI gives a
	 * 65539-byte REQUEST_URI; both answered 502 before this check. The caller
	 * turns the flag into a 400 and throws the connection away. */
	if (pair_len > FCGI_MAX_RECORD_LEN) {
		c->params_oversize = 1;
		return;
	}
	if (c->params.s && ZSTR_LEN(c->params.s) + pair_len > FCGI_MAX_RECORD_LEN) {
		fpm_http_fcgi_record(&c->out, FCGI_PARAMS, ZSTR_VAL(c->params.s), ZSTR_LEN(c->params.s));
		smart_str_free(&c->params);
	}
	fpm_http_fcgi_len(&c->params, name_len);
	fpm_http_fcgi_len(&c->params, value_len);
	smart_str_appendl(&c->params, name, name_len);
	smart_str_appendl(&c->params, value, value_len);
}

/* ---------------------------------------------------------------- request -> FastCGI */

static const char *fpm_http_method_name(enum evhttp_cmd_type type)
{
	switch (type) {
		case EVHTTP_REQ_GET: return "GET";
		case EVHTTP_REQ_POST: return "POST";
		case EVHTTP_REQ_HEAD: return "HEAD";
		case EVHTTP_REQ_PUT: return "PUT";
		case EVHTTP_REQ_DELETE: return "DELETE";
		case EVHTTP_REQ_OPTIONS: return "OPTIONS";
		case EVHTTP_REQ_TRACE: return "TRACE";
		case EVHTTP_REQ_CONNECT: return "CONNECT";
		case EVHTTP_REQ_PATCH: return "PATCH";
	}
	return NULL;
}

/* Builds BEGIN_REQUEST, PARAMS and STDIN in c->out. Returns an HTTP error code or 0.
 * script_missing_hint is fpm_http_try_local()'s realpath()/fstat() verdict on the
 * exact path this function would otherwise stat() itself (0 = exists as a regular
 * file, 1 = confirmed missing or a directory, -1 = not checked there) -- see the
 * comment on fpm_http_serve_static(). */
static int fpm_http_build_request(fpm_http_conn *c, int script_missing_hint)
{
	static const char begin_request[8] = {0, FCGI_RESPONDER, FCGI_KEEP_CONN, 0, 0, 0, 0, 0};
	struct evhttp_request *req = c->req;
	const struct evhttp_uri *uri = evhttp_request_get_evhttp_uri(req);
	const char *method = fpm_http_method_name(evhttp_request_get_command(req));
	const char *path = uri ? evhttp_uri_get_path(uri) : NULL;
	const char *query = uri ? evhttp_uri_get_query(uri) : NULL;
	const char *host = evhttp_request_get_host(req);
	struct evkeyval *header;
	struct evbuffer *body = evhttp_request_get_input_buffer(req);
	char *decoded, buf[64];
	smart_str filename = {0};
	const char *path_info;
	int trailing_slash;
	size_t decoded_len, body_len = evbuffer_get_length(body);

	if (!method || !path || !*path) {
		return HTTP_BADREQUEST;
	}

	/* Header names are bounded here, once, before anything is derived from
	 * them -- the same bound and the same 400, up front, as
	 * fpm_http_direct_request_acceptable() applies for HTTP-direct. The
	 * gateway used to append a name of any length into an unbounded smart_str
	 * and rely on whatever libevent happened to allow: it never calls
	 * evhttp_set_max_headers_size(), and libevent's default for it is
	 * EV_SIZE_MAX (libevent 2.1.12-stable, http.c:3678 in
	 * evhttp_new_object()), so there was no bound to state. "Once" is once per
	 * request that becomes FastCGI: a static file answered by
	 * fpm_http_try_local() never gets here, and derives no HTTP_* key either.
	 * Issue #115. */
	TAILQ_FOREACH(header, evhttp_request_get_input_headers(req), next) {
		if (strlen(header->key) > FPM_HTTP_HEADER_NAME_MAX) {
			return HTTP_BADREQUEST;
		}
	}

	/* SCRIPT_NAME is the decoded path, SCRIPT_FILENAME puts it under the document root */
	decoded = evhttp_uridecode(path, 0, &decoded_len);
	if (!decoded) {
		return HTTP_BADREQUEST;
	}
	if (decoded_len != strlen(decoded) || /* embedded NUL */
	    strstr(decoded, "/../") ||
	    (decoded_len >= 3 && memcmp(decoded + decoded_len - 3, "/..", 3) == 0)) {
		free(decoded);
		return HTTP_BADREQUEST;
	}
	/* Split like nginx' fastcgi_split_path_info ^(.+?\.php)(/.*)$: everything up to and
	 * including the first ".php" is the script, the rest is PATH_INFO. Doing it here rather
	 * than letting FPM stat its way to the script keeps one syscall out of every request. */
	path_info = strstr(decoded, ".php/");
	if (path_info) {
		path_info += sizeof(".php") - 1;
	}
	trailing_slash = (decoded[decoded_len - 1] == '/');
	smart_str_appends(&filename, c->gw->docroot);
	smart_str_appendl(&filename, decoded, path_info ? (size_t)(path_info - decoded) : decoded_len);
	if (trailing_slash) {
		smart_str_appendl(&filename, "index.php", sizeof("index.php") - 1);
	}
	smart_str_0(&filename);

	/* try_files $uri http.front_controller$is_args$args, roughly: when the script
	 * this request maps to does not exist -- or exists only as a directory with
	 * no index.php of its own, e.g. "/somedir" -- hand it to the front controller
	 * instead and let PATH_INFO carry the original path -- same idea as php -S's
	 * own fallback to index.php (php_cli_server_request_translate_vpath()), minus
	 * its walk-left-and-stat() loop, which is fine for a dev server but too many
	 * syscalls per request for here. (Task 018 gap 1: this is a deliberate
	 * difference from php -S, which additionally tries index.html; matching
	 * nginx's try_files instead costs no extra syscall, see below.)
	 *
	 * Cost: when path_info is NULL and there was no trailing slash (the plain
	 * "/mix" case, no .php split or appended index.php), fpm_http_try_local()
	 * already ran this exact realpath()+fstat() for GET/HEAD with http.static on,
	 * so script_missing_hint answers it for free -- directory or not. Everywhere
	 * else -- POST/PUT/..., http.static = 0, a request for a bare .php file
	 * (fpm_http_serve_static() steps aside for those on purpose), or a
	 * trailing-slash/.php-split path -- this is the one extra stat() the
	 * fallback adds, and only when http.front_controller is non-empty in the
	 * first place. */
	if (c->gw->front_controller_ok) {
		int missing;

		if (!path_info && !trailing_slash && script_missing_hint >= 0) {
			missing = script_missing_hint;
		} else {
			struct stat st;

			/* S_ISDIR counts as missing too: an existing directory with no
			 * index is the same "nothing to serve here" case as a missing
			 * file -- see the front-controller fallback's directory handling
			 * in fpm_http_serve_static(), which this stat() mirrors for the
			 * requests that don't go through that function (non-GET/HEAD,
			 * http.static = 0, or a trailing-slash/.php-split path). */
			missing = (stat(ZSTR_VAL(filename.s), &st) != 0 || S_ISDIR(st.st_mode));
		}
		if (missing) {
			smart_str_free(&filename);
			smart_str_appends(&filename, c->gw->docroot);
			smart_str_appends(&filename, c->gw->front_controller);
			smart_str_0(&filename);
			path_info = decoded;	/* whole original path, whatever split/index.php rule ran above */
		}
	}

	/* BEGIN_REQUEST goes out before the first parameter, not after the last
	 * one: fpm_http_param() flushes a full PARAMS record straight into c->out
	 * as soon as one fills up, so writing BEGIN_REQUEST at the end put it
	 * *after* those records on the wire. The worker read PARAMS as the first
	 * record of a request, fell through fcgi_read_request()'s BEGIN_REQUEST
	 * check and closed -- 502 for every request whose parameters did not fit
	 * one record. Latent until issue #117 made a 64 KiB header block a
	 * supported input; measured on 192.168.8.50, 2026-09-09: a 65000-byte
	 * block produced PARAMS(65083), PARAMS(898), PARAMS(0) with BEGIN_REQUEST
	 * in the middle. The one thing that can still fail after this point is the
	 * oversized-pair check below, and that path frees the connection with
	 * c->out unsent (fpm_http_request() -> fpm_http_conn_free()), so a
	 * half-built buffer never reaches a worker.
	 *
	 * The PARAMS records themselves are still assembled in c->params and
	 * appended below, because the last one is only complete at the end. */
	fpm_http_fcgi_record(&c->out, FCGI_BEGIN_REQUEST, begin_request, sizeof(begin_request));

	snprintf(buf, sizeof(buf), "HTTP/%d.%d", req->major, req->minor);
	fpm_http_param(c, "REQUEST_METHOD", method);
	fpm_http_param(c, "SERVER_PROTOCOL", buf);
	fpm_http_param(c, "GATEWAY_INTERFACE", "CGI/1.1");
	fpm_http_param(c, "SERVER_SOFTWARE", "PHP-FPM/" PHP_VERSION);
	fpm_http_param(c, "REQUEST_URI", evhttp_request_get_uri(req));
	fpm_http_param(c, "QUERY_STRING", query ? query : "");
	fpm_http_param(c, "DOCUMENT_ROOT", c->gw->docroot);
	fpm_http_param(c, "SCRIPT_NAME", ZSTR_VAL(filename.s) + strlen(c->gw->docroot));
	fpm_http_param(c, "SCRIPT_FILENAME", ZSTR_VAL(filename.s));
	if (path_info) {
		fpm_http_param(c, "PATH_INFO", path_info);
	}
	smart_str_free(&filename);
	free(decoded);

	if (host) {
		char *server_name = strdup(host), *colon = strrchr(server_name, ':');

		if (colon && !strchr(colon, ']')) {
			*colon = '\0';
		}
		fpm_http_param(c, "SERVER_NAME", server_name);
		free(server_name);
	}
	/* c->peer_addr/peer_port and c->fwd were resolved once by the caller
	 * (fpm_http_request()), which also decided -- via http.trusted_proxies --
	 * whether X-Forwarded-For/-Proto/-Port apply. See fpm_http_forwarded.h. */
	if (c->peer_addr[0]) {
		strlcpy(c->remote_addr, c->fwd.remote_addr[0] ? c->fwd.remote_addr : c->peer_addr, sizeof(c->remote_addr));
		fpm_http_param(c, "REMOTE_ADDR", c->remote_addr);
		snprintf(buf, sizeof(buf), "%u", (unsigned) c->peer_port);
		fpm_http_param(c, "REMOTE_PORT", buf);
	}

	/* SERVER_ADDR/SERVER_PORT: real local endpoint of this connection (not the
	 * pool's possibly-wildcard listen address), X-Forwarded-Port wins for the
	 * port when the connection is from a trusted proxy (see fpm_http_forwarded.h).
	 * SERVER_PORT is the one CGI var frameworks lean on hardest to build
	 * absolute URLs (Symfony, Laravel), hence no silent fallback to "nothing". */
	{
		char local_addr[FPM_HTTP_FORWARDED_ADDR_LEN] = "", local_port[FPM_HTTP_FORWARDED_PORT_LEN] = "";
		const char *server_port;

		fpm_http_local_addr(c->evcon, local_addr, sizeof(local_addr), local_port, sizeof(local_port));
		server_port = c->fwd.server_port[0] ? c->fwd.server_port : local_port;
		if (local_addr[0]) {
			fpm_http_param(c, "SERVER_ADDR", local_addr);
		}
		if (server_port[0]) {
			fpm_http_param(c, "SERVER_PORT", server_port);
		}
	}

	/* HTTPS/REQUEST_SCHEME: "http"/unset unless either the gateway terminated
	 * TLS on this connection itself (http.tls_cert, see fpm_http_tls.h -- that
	 * overrides the headers in fpm_http_request()) or a trusted proxy in front
	 * said so via X-Forwarded-Proto. */
	fpm_http_param(c, "REQUEST_SCHEME", c->fwd.scheme);
	if (c->fwd.https) {
		fpm_http_param(c, "HTTPS", "on");
	}

	/* AUTH_TYPE/REMOTE_USER: the gateway does not authenticate anything itself,
	 * it only relays what arrived in Authorization -- see fpm_http_auth.h. The
	 * raw header also comes through below as HTTP_AUTHORIZATION, same as nginx. */
	{
		const char *authorization = evhttp_find_header(evhttp_request_get_input_headers(req), "Authorization");
		char auth_type[FPM_HTTP_AUTH_TYPE_LEN];

		fpm_http_auth_parse(authorization, auth_type, c->remote_user);
		if (auth_type[0]) {
			fpm_http_param(c, "AUTH_TYPE", auth_type);
		}
		if (c->remote_user[0]) {
			fpm_http_param(c, "REMOTE_USER", c->remote_user);
		}
	}

	/* the body is complete (and de-chunked) at this point, so the length is ours to state */
	snprintf(buf, sizeof(buf), "%zu", body_len);
	fpm_http_param(c, "CONTENT_LENGTH", buf);

	/* "Content-Type: x" -> CONTENT_TYPE, anything else -> HTTP_<UPPER_WITH_UNDERSCORES> */
	TAILQ_FOREACH(header, evhttp_request_get_input_headers(req), next) {
		smart_str name = {0};
		const char *k = header->key;

		/* Content-Length is already above under its CGI name; "Proxy" has no
		 * CGI meaning at all and HTTP_PROXY is read as an outbound proxy by
		 * several client libraries (httpoxy, CVE-2016-5385), which is why
		 * fpm_http_direct_build_env() refuses it too. Core deletes the key
		 * from $_SERVER again -- or, when the process itself has an HTTP_PROXY
		 * environment variable, overwrites it with that value
		 * (check_http_proxy(), main/php_variables.c:861-874 in php-8.5.9) --
		 * so the gateway was not exploitable through $_SERVER before this
		 * line: measured on 192.168.8.50, 2026-09-09, `Proxy: attacker`
		 * produced no HTTP_PROXY on either pool type. It still reached the
		 * worker as a FastCGI parameter though, and getallheaders() reads
		 * those directly, not $_SERVER (sapi/fpm/fpm_main.c
		 * PHP_FUNCTION(apache_request_headers) -> fcgi_loadenv): same box, a
		 * gateway built without this exclusion answered
		 * {"proxy":"attacker", ...} with $_SERVER['HTTP_PROXY'] absent. The
		 * exclusion is here so that a header this transport's sibling refuses
		 * by name does not arrive because someone else's mitigation happens to
		 * cover one of the ways to read it. Issue #115. */
		if (strcasecmp(k, "Content-Length") == 0 || strcasecmp(k, "Proxy") == 0) {
			continue;
		}
		if (strcasecmp(k, "Content-Type") != 0) {
			smart_str_appendl(&name, "HTTP_", sizeof("HTTP_") - 1);
		}
		/* Explicit range, not toupper(): the CGI key a header lands under is a
		 * security boundary -- the Content-Length exclusion above is enforced by
		 * name -- so the mapping must not depend on LC_CTYPE. Measured on
		 * 192.168.8.50, glibc 2.43, 2026-09-09: in tr_TR.UTF-8 and az_AZ.UTF-8
		 * toupper('i') returns 'i' (the Turkish capital of 'i' is U+0130, which
		 * does not fit the single-byte table), so "If-Modified-Since" would
		 * become HTTP_IF_MODiFiED_SiNCE; de_DE.ISO-8859-1 remaps 30 bytes above
		 * 0x7F. Nothing calls setlocale() in the gateway process today -- it
		 * translates HTTP to FastCGI and never executes application PHP -- but
		 * that is an argument about when this code runs, not about what it
		 * computes. Same mapping as fpm_http_direct_build_env(), issue #109;
		 * #105 replaced the identical construct there. */
		for (; *k; k++) {
			unsigned char ch = (unsigned char) *k;	/* not `c`: that is this connection */

			if (ch >= 'a' && ch <= 'z') {
				smart_str_appendc(&name, (char) (ch - ('a' - 'A')));
			} else {
				smart_str_appendc(&name, ch == '-' ? '_' : (char) ch);
			}
		}
		smart_str_0(&name);
		fpm_http_param(c, ZSTR_VAL(name.s), header->value);
		smart_str_free(&name);
	}

	if (c->params_oversize) {
		return HTTP_BADREQUEST;
	}

	/* BEGIN_REQUEST (written above), PARAMS (possibly several records, the
	 * earlier ones already flushed by fpm_http_param()), empty PARAMS, STDIN,
	 * empty STDIN */
	if (c->params.s) {
		fpm_http_fcgi_record(&c->out, FCGI_PARAMS, ZSTR_VAL(c->params.s), ZSTR_LEN(c->params.s));
	}
	fpm_http_fcgi_record(&c->out, FCGI_PARAMS, "", 0);
	if (body_len) {
		const char *data = (const char*)evbuffer_pullup(body, -1);
		size_t off;

		for (off = 0; off < body_len; off += FCGI_MAX_RECORD_LEN) {
			fpm_http_fcgi_record(&c->out, FCGI_STDIN, data + off, MIN(body_len - off, FCGI_MAX_RECORD_LEN));
		}
	}
	fpm_http_fcgi_record(&c->out, FCGI_STDIN, "", 0);
	return 0;
}

/* ---------------------------------------------------------------- FastCGI -> response */

static void fpm_http_conn_free(fpm_http_conn *c)
{
	if (c->evcon) {
		evhttp_connection_set_closecb(c->evcon, NULL, NULL);
	}
	if (c->queued) {
		TAILQ_REMOVE(&c->gw->waiting, c, link);
	}
	smart_str_free(&c->params);
	smart_str_free(&c->out);
	smart_str_free(&c->cgi_headers);
	free(c);
}

/* The CGI header block is complete: "Status:" becomes the status line, the rest is copied. */
static void fpm_http_start_reply(fpm_http_conn *c, size_t head_len, size_t body_off)
{
	struct evkeyvalq *out = evhttp_request_get_output_headers(c->req);
	const char *line = c->cgi_headers.s ? ZSTR_VAL(c->cgi_headers.s) : "", *end = line + head_len;
	char *reason = NULL;
	int code = HTTP_OK;

	while (line < end) {
		const char *nl = memchr(line, '\n', end - line), *next = nl ? nl + 1 : end, *colon;
		size_t len = (nl ? nl : end) - line;

		if (len && line[len - 1] == '\r') {
			len--;
		}
		if ((colon = memchr(line, ':', len))) {
			size_t klen = colon - line;
			const char *v = colon + 1;
			size_t vlen = len - klen - 1;
			char *key, *value;

			while (vlen && (*v == ' ' || *v == '\t')) {
				v++;
				vlen--;
			}
			key = strndup(line, klen);
			value = strndup(v, vlen);
			if (strcasecmp(key, "Status") == 0) {
				code = atoi(value);
				free(reason);
				reason = strdup(strchr(value, ' ') ? strchr(value, ' ') + 1 : "");
			} else {
				evhttp_add_header(out, key, value);
			}
			free(key);
			free(value);
		}
		line = next;
	}

	evhttp_send_reply_start(c->req, code, reason && *reason ? reason : NULL);
	free(reason);
	c->headers_sent = 1;
	c->status = code; /* for the access log, see fpm_http_finish() */

	if (c->cgi_headers.s && body_off < ZSTR_LEN(c->cgi_headers.s)) {
		struct evbuffer *chunk = evbuffer_new();
		size_t chunk_len = ZSTR_LEN(c->cgi_headers.s) - body_off;

		evbuffer_add(chunk, ZSTR_VAL(c->cgi_headers.s) + body_off, chunk_len);
		evhttp_send_reply_chunk(c->req, chunk);
		evbuffer_free(chunk);
		c->bytes_out += chunk_len;
	}
	smart_str_free(&c->cgi_headers);
}

static void fpm_http_stdout(fpm_http_conn *c, const char *data, size_t len)
{
	size_t scan_from, i;
	const char *h;

	if (c->headers_sent) {
		struct evbuffer *chunk = evbuffer_new();

		evbuffer_add(chunk, data, len);
		evhttp_send_reply_chunk(c->req, chunk);
		evbuffer_free(chunk);
		c->bytes_out += len;
		return;
	}

	scan_from = c->cgi_headers.s && ZSTR_LEN(c->cgi_headers.s) > 3 ? ZSTR_LEN(c->cgi_headers.s) - 3 : 0;
	smart_str_appendl(&c->cgi_headers, data, len);
	smart_str_0(&c->cgi_headers);
	h = ZSTR_VAL(c->cgi_headers.s);

	for (i = scan_from; i + 1 < ZSTR_LEN(c->cgi_headers.s); i++) {
		if (h[i] == '\n' && h[i + 1] == '\n') {
			fpm_http_start_reply(c, i + 1, i + 2);
			return;
		}
		if (i + 3 < ZSTR_LEN(c->cgi_headers.s) && memcmp(h + i, "\r\n\r\n", 4) == 0) {
			fpm_http_start_reply(c, i + 2, i + 4);
			return;
		}
	}
	if (ZSTR_LEN(c->cgi_headers.s) > FPM_HTTP_MAX_CGI_HEADERS) {
		fpm_http_start_reply(c, 0, 0); /* no header block in sight, ship it as a body */
	}
}

/* The pool is done with the request (END_REQUEST seen or the connection failed).
 * `explained` says the reason is already in the log -- a clean EOF needs no
 * line at all, and fpm_http_upstream_fail() writes its own for the case it can
 * name (issue #118) -- so only the unexplained loss is reported from here. */
static void fpm_http_finish(fpm_http_conn *c, int explained)
{
	if (c->headers_sent) {
		evhttp_send_reply_end(c->req);
	} else if (c->cgi_headers.s) {
		fpm_http_start_reply(c, 0, 0); /* partial header block, ship what we have */
		evhttp_send_reply_end(c->req);
	} else {
		if (!explained) {
			zlog(ZLOG_WARNING, "[pool %s] http: no answer from '%s'", c->gw->pool, c->gw->listen_address);
		}
		c->status = FPM_HTTP_BAD_GATEWAY;
		evhttp_send_error(c->req, FPM_HTTP_BAD_GATEWAY, "Bad Gateway");
	}
	fpm_http_log_response(c->gw, c->req, c->remote_addr[0] ? c->remote_addr : c->peer_addr,
		c->remote_user, c->status, c->bytes_out);
	fpm_http_conn_free(c);
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
	struct fpm_http_gateway_s *gw = up->gw;

	TAILQ_REMOVE(&gw->upstreams, up, link);
	gw->nupstreams--;
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
	fpm_http_budget_give_back(gw);
}

static void fpm_http_upstream_free(fpm_http_upstream *up)
{
	if (up->ev_read) {
		event_free(up->ev_read);
	}
	if (up->ev_write) {
		event_free(up->ev_write);
	}
	free(up);
}

static void fpm_http_upstream_drop(fpm_http_upstream *up)
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
static void fpm_http_upstream_fail(fpm_http_upstream *up, int clean_eof)
{
	struct fpm_http_gateway_s *gw = up->gw;
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
			gw->pool, gw->listen_address, ms, clean_eof ? "EOF" : strerror(err));
	} else if (!clean_eof) {
		zlog(ZLOG_WARNING, "[pool %s] http: upstream '%s': %s", gw->pool, gw->listen_address, strerror(err));
	}
	/* EOF is normal after pm.max_requests or a worker restart; a request in flight is lost though */
	if (up->current) {
		fpm_http_finish(up->current, clean_eof || mute);
		up->current = NULL;
	}
	fpm_http_upstream_drop(up);
	fpm_http_pump(gw);
}

/* one request finished on this connection, it is free for the next */
static void fpm_http_request_done(fpm_http_upstream *up)
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
static void fpm_http_upstream_flush(fpm_http_upstream *up)
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

static void fpm_http_upstream_write(fpm_http_upstream *up, const char *data, size_t len)
{
	smart_str_appendl(&up->pending, data, len);
	if (!up->connecting) {
		fpm_http_upstream_flush(up);
	}
}

static fpm_http_upstream *fpm_http_upstream_new(struct fpm_http_gateway_s *gw)
{
	fpm_http_upstream *up;

	if (!fpm_http_budget_take(gw)) {
		return NULL;
	}
	up = calloc(1, sizeof(*up));
	up->gw = gw;
	up->fd = socket(gw->upstream_addr.ss_family, SOCK_STREAM, 0);
	if (up->fd < 0) {
		free(up);
		fpm_http_budget_give_back(gw);
		return NULL;
	}
	evutil_make_socket_nonblocking(up->fd);
	if (gw->upstream_addr.ss_family != AF_UNIX) {
		int on = 1;

		setsockopt(up->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
	}
	up->ev_read = event_new(gw->base, up->fd, EV_READ | EV_PERSIST, fpm_http_upstream_readcb, up);
	up->ev_write = event_new(gw->base, up->fd, EV_WRITE, fpm_http_upstream_writecb, up);

	if (connect(up->fd, (struct sockaddr*)&gw->upstream_addr, gw->upstream_len) != 0) {
		if (errno != EINPROGRESS) {
			event_free(up->ev_read);
			event_free(up->ev_write);
			close(up->fd);
			free(up);
			fpm_http_budget_give_back(gw);
			return NULL;
		}
		up->connecting = 1;
		event_add(up->ev_write, NULL);
	} else {
		event_add(up->ev_read, NULL);
	}
	TAILQ_INSERT_TAIL(&gw->upstreams, up, link);
	gw->nupstreams++;
	return up;
}

/* One dispatch round. Never called directly -- fpm_http_pump() below owns the
 * re-entrancy rules. */
static void fpm_http_pump_once(struct fpm_http_gateway_s *gw)
{
	while (!TAILQ_EMPTY(&gw->waiting)) {
		fpm_http_upstream *up, *idle = NULL;
		fpm_http_conn *c;
		smart_str out;

		TAILQ_FOREACH(up, &gw->upstreams, link) {
			if (!up->busy) {
				idle = up;
				break;
			}
		}
		if (!idle) {
			idle = fpm_http_upstream_new(gw);
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
			 * a pointer to. */
			while (!TAILQ_EMPTY(&gw->waiting)) {
				c = TAILQ_FIRST(&gw->waiting);
				TAILQ_REMOVE(&gw->waiting, c, link);
				c->queued = 0;
				c->status = FPM_HTTP_SERVICE_UNAVAIL;
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
					c->remote_user, c->status, c->bytes_out);
				fpm_http_conn_free(c);
			}
			return;
		}

		c = TAILQ_FIRST(&gw->waiting);
		TAILQ_REMOVE(&gw->waiting, c, link);
		c->queued = 0;
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

/* Hands waiting requests to free connections, opening new ones up to this
 * process' share. Re-entrant: the dispatch can fail synchronously and the
 * failure path calls back in here, see gw->pumping. */
static void fpm_http_pump(struct fpm_http_gateway_s *gw)
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

/* the client went away: stop writing to it, but let the pool finish so the connection stays usable */
static void fpm_http_client_closed(struct evhttp_connection *evcon, void *arg)
{
	fpm_http_conn *c = arg;

	c->evcon = NULL;
	if (c->upstream) {
		c->upstream->current = NULL;
		c->upstream = NULL;
	}
	fpm_http_conn_free(c);
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

static const struct {
	const char *ext;
	const char *type;
} fpm_http_mime[] = {
	{ "html", "text/html; charset=UTF-8" },
	{ "htm",  "text/html; charset=UTF-8" },
	{ "css",  "text/css; charset=UTF-8" },
	{ "js",   "text/javascript; charset=UTF-8" },
	{ "mjs",  "text/javascript; charset=UTF-8" },
	{ "json", "application/json" },
	{ "map",  "application/json" },
	{ "xml",  "application/xml" },
	{ "txt",  "text/plain; charset=UTF-8" },
	{ "svg",  "image/svg+xml" },
	{ "png",  "image/png" },
	{ "jpg",  "image/jpeg" },
	{ "jpeg", "image/jpeg" },
	{ "gif",  "image/gif" },
	{ "webp", "image/webp" },
	{ "avif", "image/avif" },
	{ "ico",  "image/x-icon" },
	{ "woff", "font/woff" },
	{ "woff2","font/woff2" },
	{ "ttf",  "font/ttf" },
	{ "otf",  "font/otf" },
	{ "pdf",  "application/pdf" },
	{ "wasm", "application/wasm" },
	{ "mp4",  "video/mp4" },
	{ "webm", "video/webm" },
	{ NULL, NULL }
};

static const char *fpm_http_content_type(const char *path)
{
	const char *dot = strrchr(path, '.');
	unsigned i;

	if (!dot || strchr(dot, '/')) {
		return "application/octet-stream";
	}
	dot++;
	for (i = 0; fpm_http_mime[i].ext; i++) {
		if (!strcasecmp(dot, fpm_http_mime[i].ext)) {
			return fpm_http_mime[i].type;
		}
	}

	return "application/octet-stream";
}

/* Any path segment starting with a dot is refused: .env, .git, .htaccess and
 * friends must never be served just because they sit under the document root.
 * nginx needs an explicit rule for this; we make it the default. */
static int fpm_http_path_has_dotfile(const char *path)
{
	const char *p = path;

	while ((p = strchr(p, '/')) != NULL) {
		if (p[1] == '.') {
			return 1;
		}
		p++;
	}

	return 0;
}

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
 * already decided, exactly like fpm_http_tls_validate() decides TLS cert/key
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
static void fpm_http_front_controller_validate(struct fpm_http_gateway_s *gw)
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
static int fpm_http_serve_static(struct fpm_http_gateway_s *gw, struct evhttp_request *req,
		const char *path, size_t path_len, const char *remote_addr, int *script_missing)
{
	char candidate[MAXPATHLEN], resolved[MAXPATHLEN], etag[64];
	const char *root, *inm;
	struct evkeyvalq *out;
	struct stat st;
	size_t root_len;
	int fd, cmd;

	cmd = evhttp_request_get_command(req);
	if (cmd != EVHTTP_REQ_GET && cmd != EVHTTP_REQ_HEAD) {
		return 0;
	}
	/* Anything that is a PHP script, or has PATH_INFO behind one, is the
	 * worker's business. A directory falls through to index.php too. */
	if (!path_len || path[path_len - 1] == '/' || strstr(path, ".php/")) {
		return 0;
	}
	if (path_len >= 4 && !strcasecmp(path + path_len - 4, ".php")) {
		return 0;
	}
	if (fpm_http_path_has_dotfile(path)) {
		fpm_http_log_response(gw, req, remote_addr, NULL, HTTP_NOTFOUND, 0);
		evhttp_send_error(req, HTTP_NOTFOUND, NULL);
		return 1;
	}

	root = fpm_http_docroot_real(gw);
	if (!root) {
		return 0;
	}
	root_len = strlen(root);

	if ((size_t)snprintf(candidate, sizeof(candidate), "%s%s", root, path) >= sizeof(candidate)) {
		fpm_http_log_response(gw, req, remote_addr, NULL, HTTP_NOTFOUND, 0);
		evhttp_send_error(req, HTTP_NOTFOUND, NULL);
		return 1;
	}
	/* realpath() is the only honest containment check: the textual /../ filter
	 * in fpm_http_build_request does not catch a symlink pointing outside the
	 * document root. One extra syscall, but a static hit costs no worker at
	 * all, so the trade is easy. */
	if (!realpath(candidate, resolved)) {
		if (script_missing) {
			*script_missing = 1;	/* http.front_controller reuses this instead of stat()-ing again */
		}
		return 0;			/* no such file: let the worker produce the 404 */
	}
	if (script_missing) {
		*script_missing = 0;	/* file is there, whatever open()/fstat() below turn out to say about it */
	}
	if (strncmp(resolved, root, root_len) != 0 || (resolved[root_len] && resolved[root_len] != '/')) {
		zlog(ZLOG_NOTICE, "[pool %s] http: refused '%s' outside the document root", gw->pool, path);
		fpm_http_log_response(gw, req, remote_addr, NULL, HTTP_NOTFOUND, 0);
		evhttp_send_error(req, HTTP_NOTFOUND, NULL);
		return 1;
	}

	fd = open(resolved, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return 0;
	}
	if (fstat(fd, &st) < 0) {
		close(fd);
		return 0;
	}
	if (!S_ISREG(st.st_mode)) {
		close(fd);
		/* A directory with no index.php is "nothing to serve" the same way a
		 * missing file is: reuse this fstat() -- already paid for -- to tell
		 * fpm_http_build_request() so, instead of it needing a stat() of its
		 * own. Other non-regular types (sockets, devices, ...) are left as
		 * before: the worker's business, not the front controller's. */
		if (script_missing && S_ISDIR(st.st_mode)) {
			*script_missing = 1;
		}
		return 0;
	}

	snprintf(etag, sizeof(etag), "\"%llx-%llx\"",
		(unsigned long long)st.st_mtime, (unsigned long long)st.st_size);

	out = evhttp_request_get_output_headers(req);
	inm = evhttp_find_header(evhttp_request_get_input_headers(req), "If-None-Match");
	if (inm && !strcmp(inm, etag)) {
		close(fd);
		evhttp_add_header(out, "ETag", etag);
		fpm_http_log_response(gw, req, remote_addr, NULL, 304, 0);
		evhttp_send_reply(req, 304, "Not Modified", NULL);
		return 1;
	}

	evhttp_add_header(out, "Content-Type", fpm_http_content_type(resolved));
	evhttp_add_header(out, "ETag", etag);

	if (cmd == EVHTTP_REQ_HEAD) {
		char len[32];

		close(fd);
		snprintf(len, sizeof(len), "%llu", (unsigned long long)st.st_size);
		evhttp_add_header(out, "Content-Length", len);
		/* HEAD sends no body, log 0 bytes like nginx' $body_bytes_sent would */
		fpm_http_log_response(gw, req, remote_addr, NULL, HTTP_OK, 0);
		evhttp_send_reply(req, HTTP_OK, "OK", NULL);
		return 1;
	}

	/* evbuffer_add_file takes ownership of fd and uses sendfile/mmap where it
	 * can, so the bytes never pass through our address space. */
	{
		struct evbuffer *body = evbuffer_new();

		if (!body || evbuffer_add_file(body, fd, 0, st.st_size) < 0) {
			if (body) {
				evbuffer_free(body);
			} else {
				close(fd);
			}
			fpm_http_log_response(gw, req, remote_addr, NULL, FPM_HTTP_BAD_GATEWAY, 0);
			evhttp_send_error(req, FPM_HTTP_BAD_GATEWAY, "Bad Gateway");
			return 1;
		}
		fpm_http_log_response(gw, req, remote_addr, NULL, HTTP_OK, (size_t) st.st_size);
		evhttp_send_reply(req, HTTP_OK, "OK", body);
		evbuffer_free(body);
	}

	return 1;
}

/* Returns 1 when the gateway answered on its own; 0 to hand the request to a
 * worker. *script_missing carries fpm_http_serve_static()'s realpath() result
 * out (see the comment there) so fpm_http_build_request() can reuse it. */
static int fpm_http_try_local(struct fpm_http_gateway_s *gw, struct evhttp_request *req, const char *remote_addr, int *script_missing)
{
	const char *uri = evhttp_request_get_uri(req);
	const struct evhttp_uri *decoded_uri;
	const char *raw_path;
	char *path;
	size_t path_len;
	int answered = 0;

	if (!gw->static_files || !uri) {
		return 0;
	}

	decoded_uri = evhttp_request_get_evhttp_uri(req);
	raw_path = decoded_uri ? evhttp_uri_get_path(decoded_uri) : NULL;
	if (!raw_path || !*raw_path) {
		return 0;
	}

	path = evhttp_uridecode(raw_path, 0, &path_len);
	if (!path) {
		return 0;
	}
	if (path_len != strlen(path) || path[0] != '/' || strstr(path, "/../") ||
	    (path_len >= 3 && !memcmp(path + path_len - 3, "/..", 3))) {
		free(path);
		return 0;			/* let fpm_http_build_request produce the 400 */
	}

	/* Order will matter once ACME and /status arrive: fixed-path things
	 * first, files from disk only at the end. */
	answered = fpm_http_serve_static(gw, req, path, path_len, remote_addr, script_missing);

	free(path);

	return answered;
}

static int fpm_http_is_acme_challenge(struct evhttp_request *req)
{
	const struct evhttp_uri *uri = evhttp_request_get_evhttp_uri(req);
	const char *path = uri ? evhttp_uri_get_path(uri) : NULL;
	static const char prefix[] = "/.well-known/acme-challenge/";

	return path && !strncmp(path, prefix, sizeof(prefix) - 1);
}

static void fpm_http_plain_request(struct evhttp_request *req, void *arg)
{
	const char *host = evhttp_find_header(evhttp_request_get_input_headers(req), "Host");
	const char *uri = evhttp_request_get_uri(req);
	struct evkeyvalq *headers = evhttp_request_get_output_headers(req);
	char *location;
	char *redirect_host = NULL;
	size_t len;

	(void)arg;
	if (fpm_http_is_acme_challenge(req)) {
		evhttp_send_error(req, HTTP_NOTFOUND, "ACME challenge is not provisioned");
		return;
	}
	if (!host || !*host || strchr(host, '\r') || strchr(host, '\n') || !uri) {
		evhttp_send_error(req, HTTP_BADREQUEST, "Bad Request");
		return;
	}
	if (host[0] == '[') {
		const char *end = strchr(host, ']');

		if (!end || (end[1] && end[1] != ':')) {
			evhttp_send_error(req, HTTP_BADREQUEST, "Bad Request");
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
		return;
	}

	len = sizeof("https://") - 1 + strlen(redirect_host) + strlen(uri) + 1;
	location = malloc(len);
	if (!location) {
		free(redirect_host);
		evhttp_send_error(req, HTTP_SERVUNAVAIL, "Service Unavailable");
		return;
	}
	snprintf(location, len, "https://%s%s", redirect_host, uri);
	evhttp_add_header(headers, "Location", location);
	evhttp_send_reply(req, 308, "Permanent Redirect", NULL);
	free(location);
	free(redirect_host);
}

static void fpm_http_request(struct evhttp_request *req, void *arg)
{
	struct fpm_http_gateway_s *gw = arg;
	struct evhttp_connection *evcon = evhttp_request_get_connection(req);
	char *peer_addr = NULL;
	ev_uint16_t peer_port = 0;
	struct fpm_http_forwarded_result_s fwd;
	const char *effective_addr;
	fpm_http_conn *c;
	int error;

	if (evcon) {
		evhttp_connection_get_peer(evcon, &peer_addr, &peer_port);
	}

	/* Reaching this callback means the client delivered the whole request
	 * (evhttp buffers headers AND body before dispatching), so its read
	 * deadline (task 031, armed at accept) is spent. */
	if (gw->read_timeout_ms > 0) {
		fpm_http_read_deadline_disarm(gw, evcon ? evhttp_connection_get_bufferevent(evcon) : NULL);
	}

	if (gw->acl && !fpm_http_acl_check(gw->acl, peer_addr)) {
		/* ACL is about the direct network peer, so it (and its log entry) is
		 * deliberately NOT run through X-Forwarded-For -- an address rejected
		 * here is exactly the one that made the TCP connection. */
		fpm_http_log_response(gw, req, peer_addr, NULL, 403, 0);
		evhttp_send_error(req, 403, "Forbidden");
		return;
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

	/* Local responses first: there is no point building FastCGI parameters or
	 * occupying a worker slot for a file we will serve ourselves. -1 = "not
	 * checked" (not GET/HEAD, or http.static = 0): fpm_http_build_request() then
	 * decides whether it needs its own stat() for http.front_controller. */
	{
		int script_missing = -1;

		if (fpm_http_try_local(gw, req, effective_addr, &script_missing)) {
			return;
		}

		c = calloc(1, sizeof(*c));
		c->gw = gw;
		c->req = req;
		c->evcon = evcon;
		c->status = -1;
		c->fwd = fwd;
		c->peer_port = peer_port;
		if (peer_addr) {
			strlcpy(c->peer_addr, peer_addr, sizeof(c->peer_addr));
		}

		error = fpm_http_build_request(c, script_missing);
	}

	if (error) {
		fpm_http_log_response(gw, req, effective_addr, NULL, error, 0);
		evhttp_send_error(req, error, NULL);
		c->evcon = NULL;
		fpm_http_conn_free(c);
		return;
	}

	evhttp_connection_set_closecb(c->evcon, fpm_http_client_closed, c);
	TAILQ_INSERT_TAIL(&gw->waiting, c, link);
	c->queued = 1;
	fpm_http_pump(gw);
}

/* ---------------------------------------------------------------- processes */

static int fpm_http_resolve_upstream(struct fpm_http_gateway_s *gw)
{
	const char *address = gw->listen_address;

	if (fpm_sockets_domain_from_address(gw->listen_address) == FPM_AF_UNIX) {
		struct sockaddr_un *sa_un = (struct sockaddr_un*)&gw->upstream_addr;

		sa_un->sun_family = AF_UNIX;
		strlcpy(sa_un->sun_path, address, sizeof(sa_un->sun_path));
		gw->upstream_len = sizeof(*sa_un);
		return 0;
	} else {
		struct addrinfo hints, *res;
		char *dup_address = strdup(address), *host = NULL, *port = strrchr(dup_address, ':');
		int ret;

		if (port) {
			*port++ = '\0';
			host = dup_address;
			if (host[0] == '[' && host[strlen(host) - 1] == ']') {
				host[strlen(host) - 1] = '\0';
				host++;
			}
		} else {
			port = dup_address; /* a bare port listens on any address */
		}
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		ret = getaddrinfo(host ? host : "localhost", port, &hints, &res);
		if (ret == 0) {
			memcpy(&gw->upstream_addr, res->ai_addr, res->ai_addrlen);
			gw->upstream_len = res->ai_addrlen;
			freeaddrinfo(res);
		}
		free(dup_address);
		return ret == 0 ? 0 : -1;
	}
}

/* Drops the gateway process from the master's identity (root, in the usual
 * deployment where the master binds privileged ports) to the pool's own
 * 'user'/'group' -- the same identity fpm_unix_init_child() (fpm_unix.c) puts
 * the request workers under. Called once per gateway process, after the last
 * operation that can need root (see the call site in fpm_http_gateway_run())
 * and before the event loop ever accepts a connection.
 *
 * A pool with no 'user'/'group' at all is only possible under FPM's explicit
 * run_as_root escape hatch (fpm_unix_conf_wp() refuses it otherwise) -- the
 * operator asked for root there, so the gateway stays root too, same as a
 * worker would. Anything else -- setgid/initgroups/setuid actually failing --
 * is fatal: never continue serving TLS with the private key as root. */
static void fpm_http_gateway_drop_privileges(struct fpm_http_gateway_s *gw) /* {{{ */
{
	if (geteuid() != 0) {
		return; /* the master was not root either, nothing to drop */
	}

	if (!gw->drop_uid && !gw->drop_gid) {
		zlog(ZLOG_WARNING, "[pool %s] http gateway: pool has no user/group, gateway keeps running as root", gw->pool);
		return;
	}

	if (setgid(gw->drop_gid) != 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] http gateway: failed to setgid(%d)", gw->pool, (int) gw->drop_gid);
		exit(FPM_EXIT_SOFTWARE);
	}
	if (initgroups(gw->drop_user, gw->drop_gid) != 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] http gateway: failed to initgroups(%s, %d)", gw->pool, gw->drop_user, (int) gw->drop_gid);
		exit(FPM_EXIT_SOFTWARE);
	}
	if (setuid(gw->drop_uid) != 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] http gateway: failed to setuid(%d)", gw->pool, (int) gw->drop_uid);
		exit(FPM_EXIT_SOFTWARE);
	}
	if (geteuid() == 0) {
		/* setuid(0) target, or a libc/capability quirk that made it a no-op:
		 * either way, never serve a TLS private key as root */
		zlog(ZLOG_ERROR, "[pool %s] http gateway: still root after dropping privileges", gw->pool);
		exit(FPM_EXIT_SOFTWARE);
	}
}
/* }}} */

/* The read deadline fired while the client was still delivering its first
 * request. The connection must NOT be torn down by hand: the bufferevent is
 * owned by evhttp's evhttp_connection, and bufferevent_free() underneath it
 * is a use-after-free (this exact mistake SIGSEGVed every gateway under the
 * tls-reload load loop on the test box). Instead, shrink the connection's
 * own read timeout to (almost) zero -- bufferevent_set_timeouts() re-arms
 * the pending read event through be_ops->adj_timeouts, so the timeout fires
 * immediately and evhttp closes the connection itself, through its own
 * error path, with the connection state consistent. A connection whose read
 * is not currently armed keeps existing, which is benign: it is either idle
 * keep-alive (harmless) or about to arm read again.
 *
 * dl->bev is valid here because arm() holds a reference of its own, NOT
 * because the connection is still one evhttp owns -- it may well not be. The
 * comment that used to stand here claimed the opposite ("a fired deadline
 * always refers to a connection evhttp still owns", on the grounds that the
 * EOF watcher below would have disarmed the node otherwise) and that claim
 * was measurably false: when evhttp frees the bufferevent it closes the fd,
 * epoll drops the watcher's registration without telling libevent, the
 * watcher never fires again, and this call reached into freed memory --
 * SIGSEGV inside bufferevent_set_timeouts(), roughly http.read_timeout after
 * a burst of aborted TLS connections (issue #90, backtrace on the poligon
 * 2026-09-08). With the reference held, a deadline that fires on a
 * connection evhttp has already dropped merely re-arms a timeout nobody is
 * listening to, and the decref in forget() then closes the socket. */
static void fpm_http_read_deadline_fire(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_http_read_deadline_s *dl = arg;
	static const struct timeval now = {0, 1};

	(void) fd; (void) what;
	bufferevent_set_timeouts(dl->bev, &now, &now);
	fpm_http_read_deadline_forget(dl);
}
/* The peer closed the connection before its first request completed
 * (EV_EOF), or libevent reports the fd as dead. Since issue #90 nothing here
 * is load-bearing for safety -- the reference taken in arm() is -- but the
 * connection is over, so forgetting the node now releases that reference,
 * and with it the fd, instead of holding both for whatever is left of the
 * deadline. The watcher also sees EV_READ whenever a trickle byte arrives;
 * only EOF (a zero-length peek) disarms. */
static void fpm_http_read_deadline_eof(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_http_read_deadline_s *dl = arg;
	bufferevent_data_cb readcb = NULL;
	char c;
	ssize_t n;

	(void) fd;
	if (what & EV_READ) {
		/* bufferevent_free() clears the callbacks (libevent-2.1.12
		 * bufferevent.c:809) and our reference keeps the object and its fd
		 * alive past that point, so "no read callback" means evhttp is done
		 * with this connection and nobody will consume what is still in the
		 * socket buffer. Checked BEFORE the peek: unread bytes look exactly
		 * like a live peer to it, and on a level-triggered EV_READ that
		 * would spin the event loop for the rest of the deadline. */
		bufferevent_getcb(dl->bev, &readcb, NULL, NULL, NULL);
		if (readcb) {
			n = recv(dl->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
			/* fpm_http_would_block(), not "EAGAIN || EWOULDBLOCK": the two
			 * are the same value on Linux, which gcc reports as
			 * -Wlogical-op, and that helper already carries the
			 * #if EWOULDBLOCK != EAGAIN dance for the systems where they
			 * differ. */
			if (n > 0 || (n < 0 && fpm_http_would_block(errno))) {
				return; /* data available or transient: still alive */
			}
		}
	}
	fpm_http_read_deadline_forget(dl);
}

/* Unlinks and frees a deadline node. Safe to call twice is NOT required --
 * both callers (fire and eof) free exactly once, and disarm() removes the
 * node from the list first, so neither callback can find it afterwards. */
static void fpm_http_read_deadline_forget(struct fpm_http_read_deadline_s *dl)
{
	struct fpm_http_read_deadline_s **p;

	for (p = &dl->gw->deadlines; *p; p = &(*p)->next) {
		if (*p == dl) {
			*p = dl->next;
			break;
		}
	}
	if (dl->ev) {
		event_free(dl->ev);
	}
	if (dl->ev_eof) {
		event_free(dl->ev_eof);
	}
	/* The reference arm() took. Last, and after both events are gone: when
	 * evhttp has already let go of this connection, this is the call that
	 * frees the bufferevent and closes its fd (BEV_OPT_CLOSE_ON_FREE), and
	 * the EOF watcher must not be registered on that fd when it goes. */
	bufferevent_decref(dl->bev);
	free(dl);
}

/* Arms the per-connection read deadline (see struct fpm_http_read_deadline_s).
 * The fd is read from the bev; on a TLS connection it is not yet assigned at
 * bevcb time (bufferevent_setfd happens right after, in evhttp), so the EOF
 * watcher is armed lazily on the first event loop pass via a zero timer. */
static void fpm_http_read_deadline_arm(struct fpm_http_gateway_s *gw, struct bufferevent *bev)
{
	struct fpm_http_read_deadline_s *dl = calloc(1, sizeof(*dl));

	if (!dl) {
		return; /* OOM: degrade to libevent's idle timeout only, the listener still works */
	}
	dl->gw = gw;
	dl->bev = bev;
	dl->fd = -1;
	/* A reference of our own, released in forget(). evhttp frees this
	 * bufferevent as soon as the connection ends, which is routinely BEFORE
	 * the deadline fires; with a reference outstanding, bufferevent_free()
	 * only clears the callbacks and cancels pending operations
	 * (libevent-2.1.12 bufferevent.c:805-812), leaving the object, its
	 * events and its fd valid until the last reference goes. Every dl->bev
	 * and dl->fd use below rests on that, and nothing else -- see
	 * fpm_http_read_deadline_fire() for what happened without it (issue
	 * #90). Cost: a connection that dies before its first request keeps its
	 * fd until the deadline expires, unless the EOF watcher below gets to it
	 * first. */
	bufferevent_incref(bev);
	dl->ev = event_new(gw->base, -1, EV_TIMEOUT, fpm_http_read_deadline_fire, dl);
	dl->ev_eof = event_new(gw->base, -1, EV_TIMEOUT, fpm_http_read_deadline_arm_eof, dl);
	if (!dl->ev || !dl->ev_eof) {
		fpm_http_read_deadline_forget(dl);
		return;
	}
	dl->next = gw->deadlines;
	gw->deadlines = dl;
	event_add(dl->ev, &gw->read_timeout);
	{
		static const struct timeval zero = {0, 0};
		event_add(dl->ev_eof, &zero); /* re-armed as EV_READ once the fd is known */
	}
}

/* Second pass of arming: evhttp has called bufferevent_setfd() by now, so
 * dl->fd is knowable. Turns ev_eof into the persistent EOF watcher. The
 * bufferevent may already be gone from evhttp's point of view when this runs
 * -- a connection that fails in the same loop iteration it was accepted in
 * gets there first -- so reading its fd is safe only because of the
 * reference arm() holds. */
static void fpm_http_read_deadline_arm_eof(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_http_read_deadline_s *dl = arg;

	(void) fd; (void) what;
	dl->fd = bufferevent_getfd(dl->bev);
	if (dl->fd < 0) {
		/* still no fd (should not happen): without the watcher a peer close
		 * would leave a dangling bev, so drop the deadline entirely */
		fpm_http_read_deadline_forget(dl);
		return;
	}
	event_assign(dl->ev_eof, dl->gw->base, dl->fd, EV_READ | EV_PERSIST, fpm_http_read_deadline_eof, dl);
	event_add(dl->ev_eof, NULL);
}

/* The first request on this connection has fully arrived: its deadline is
 * spent. Safe to call when none is armed (read_timeout = 0 or OOM above). */
static void fpm_http_read_deadline_disarm(struct fpm_http_gateway_s *gw, struct bufferevent *bev)
{
	struct fpm_http_read_deadline_s **p;

	if (!bev) {
		return;
	}
	for (p = &gw->deadlines; *p; p = &(*p)->next) {
		if ((*p)->bev == bev) {
			fpm_http_read_deadline_forget(*p);
			return;
		}
	}
}

/* The gateway's bevcb: builds the bufferevent for a new client connection and
 * arms its read deadline. TLS connections get their SSL bufferevent from
 * fpm_http_tls_bevcb() with the pool's CURRENT SSL_CTX (gw->tls_ctx), so a
 * hot-reloaded certificate (fpm_http_tls_reload.c) applies to new connections
 * without this wrapper being re-registered. */
static struct bufferevent *fpm_http_bevcb(struct event_base *base, void *arg)
{
	struct fpm_http_gateway_s *gw = arg;
	struct bufferevent *bev;

#ifdef HAVE_FPM_HTTP_TLS
	if (gw->tls_ctx) {
		bev = fpm_http_tls_bevcb(base, gw->tls_ctx);
	} else
#endif
	{
		bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
	}
	if (bev && gw->read_timeout_ms > 0) {
		fpm_http_read_deadline_arm(gw, bev);
	}
	return bev;
}

static void fpm_http_gateway_run(struct fpm_http_gateway_s *gw, unsigned index) /* {{{ */
{
	struct fpm_worker_pool_s *wp;
	struct sigaction act;
	char title[128];

	fpm_globals.is_child = 1;

	/* plain defaults: the master terminates us with a signal, nothing to clean up */
	memset(&act, 0, sizeof(act));
	act.sa_handler = SIG_DFL;
	sigaction(SIGTERM, &act, 0);
	sigaction(SIGINT, &act, 0);
	sigaction(SIGQUIT, &act, 0);
	sigaction(SIGUSR1, &act, 0);
	sigaction(SIGUSR2, &act, 0);
	sigaction(SIGCHLD, &act, 0);
	act.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &act, 0);
	fpm_signals_unblock();

	/* the pools' FastCGI listeners are the master's business */
	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		close(wp->listening_socket);
	}

	snprintf(title, sizeof(title), "http gateway %s [%u]", gw->pool, index);
	fpm_env_setproctitle(title);

	if (gw->reuseport) {
		/* own listening socket in the SO_REUSEPORT group, the kernel spreads connections by hash;
		 * the last thing that can need root, so the privilege drop below waits for it */
		close(gw->listen_fd);
		gw->listen_fd = fpm_http_listen(gw->pool, gw->listen_address, gw->http_listen_override, gw->backlog, 1);
		if (gw->listen_fd < 0) {
			exit(FPM_EXIT_SOFTWARE);
		}
		if (gw->plain_listen_address) {
			close(gw->plain_listen_fd);
			gw->plain_listen_fd = fpm_http_listen(gw->pool, gw->listen_address, gw->plain_listen_address, gw->backlog, 1);
			if (gw->plain_listen_fd < 0) {
				exit(FPM_EXIT_SOFTWARE);
			}
		}
	}

	/* Everything above this line is the only reason the gateway ever needed
	 * root: binding http.reuseport's own listener, and holding the TLS
	 * private key the master read before the first fork (fpm_http_tls.h).
	 * Nothing below -- the access log, static files, TLS handshakes, proxying
	 * to the pool -- needs it. See task 010 (done; see docs/task-archive.md). */
	fpm_http_gateway_drop_privileges(gw);

	/* one fd per gateway process, all appending to the same http.access_log
	 * path -- see fpm_http_access_log.h for why that does not interleave.
	 * Opened after the drop so the file is created by the dropped-to identity. */
	gw->access_log = fpm_http_access_log_open(gw->pool, gw->access_log_path);

	if (fpm_http_resolve_upstream(gw) != 0) {
		zlog(ZLOG_ERROR, "[pool %s] http: cannot resolve '%s'", gw->pool, gw->listen_address);
		exit(FPM_EXIT_SOFTWARE);
	}
	TAILQ_INIT(&gw->upstreams);
	TAILQ_INIT(&gw->waiting);

	gw->base = event_base_new();
	gw->http = evhttp_new(gw->base);
#ifdef HAVE_FPM_HTTP_TLS
	if (gw->tls) {
		/* Own SSL_CTX per gateway process, built from cert/key bytes the
		 * master already read and validated, never from an SSL_CTX inherited
		 * through fork() -- see fpm_http_tls.h.
		 *
		 * The bytes come from the reload machinery's currently published slot
		 * whenever there is one, not from gw->tls: gw->tls is what the master
		 * read once before the FIRST fork, so a gateway respawned after N
		 * certificate reloads would otherwise start with -- and, believing it
		 * was up to date, keep -- the startup certificate (issue #91). This is
		 * the "builds its SSL_CTX from the currently published slot" branch of
		 * that issue: the respawned process is correct from its first accepted
		 * connection, with no adoption tick and no window in between. For a
		 * gateway forked at startup the published slot is generation 0, i.e.
		 * exactly gw->tls's bytes, so nothing about startup changes. */
		gw->tls_ctx = fpm_http_tls_reload_child_ctx_new(gw->reload);
		if (!gw->tls_ctx) {
			gw->tls_ctx = fpm_http_tls_ctx_new(gw->pool, gw->tls);
		}
		if (!gw->tls_ctx) {
			exit(FPM_EXIT_SOFTWARE);
		}

		/* Own generation-watch timer, on this child's own base -- see
		 * fpm_http_tls_reload.h. No-op when gw->reload is NULL. The bevcb
		 * pair keeps a reload from dropping the read deadline (task 031). */
		fpm_http_tls_reload_child_init(gw->reload, gw->base, gw->http, &gw->tls_ctx,
			fpm_http_bevcb, gw);
	}
#endif
	/* One bevcb for every listener (TLS and plain alike): it wraps the
	 * bufferevent AND arms the per-connection read deadline (task 031).
	 * evhttp's own timeout is NOT used -- a bufferevent read timeout is an
	 * idle timer restarted on every received byte, so a slow-loris client
	 * trickling one byte at a time would never trip it. See
	 * struct fpm_http_read_deadline_s. */
	evhttp_set_bevcb(gw->http, fpm_http_bevcb, gw);
	evhttp_set_allowed_methods(gw->http, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD | EVHTTP_REQ_PUT |
		EVHTTP_REQ_DELETE | EVHTTP_REQ_OPTIONS | EVHTTP_REQ_PATCH);
	evhttp_set_max_body_size(gw->http, gw->max_body);
	/* Without this the block limit is libevent's default EV_SIZE_MAX (libevent
	 * 2.1.12-stable, http.c:3678 in evhttp_new_object()): a client could send
	 * headers until the process died, and unlike a direct-transport worker
	 * this one process serves every connection of the pool, so that memory is
	 * charged against every in-flight request here. #115 bounded a header
	 * *name*, which does nothing about their number. The refusal is
	 * libevent's, not ours: over the limit it fails the connection with
	 * EVREQ_HTTP_INVALID_HEADER (http.c:2303 evhttp_read_header()), which for
	 * an incoming connection answers 400 and closes (http.c:664
	 * evhttp_connection_incoming_fail()) -- the same status our own
	 * over-long-name check returns. The request line is charged against the
	 * same budget (http.c:2041), so a pathological URI is bounded too.
	 * Issue #117. */
	evhttp_set_max_headers_size(gw->http, FPM_HTTP_HEADERS_MAX);
	evhttp_set_gencb(gw->http, fpm_http_request, gw);
	evutil_make_socket_nonblocking(gw->listen_fd);
	if (evhttp_accept_socket(gw->http, gw->listen_fd) != 0) {
		zlog(ZLOG_ERROR, "[pool %s] http: evhttp_accept_socket() failed", gw->pool);
		exit(FPM_EXIT_SOFTWARE);
	}
	if (gw->plain_listen_fd >= 0) {
		struct evhttp *plain = evhttp_new(gw->base);

		if (!plain) {
			exit(FPM_EXIT_SOFTWARE);
		}
		evhttp_set_allowed_methods(plain, EVHTTP_REQ_GET | EVHTTP_REQ_HEAD);
		evhttp_set_max_body_size(plain, 0);
		/* Same bound on http.plain_listen: it is the same process and the same
		 * unauthenticated listener, and it answers before TLS, so leaving it
		 * at EV_SIZE_MAX would leave the hole open on the easier port. */
		evhttp_set_max_headers_size(plain, FPM_HTTP_HEADERS_MAX);
		evhttp_set_gencb(plain, fpm_http_plain_request, gw);
		evutil_make_socket_nonblocking(gw->plain_listen_fd);
		if (evhttp_accept_socket(plain, gw->plain_listen_fd) != 0) {
			zlog(ZLOG_ERROR, "[pool %s] http: evhttp_accept_socket() failed for http.plain_listen", gw->pool);
			exit(FPM_EXIT_SOFTWARE);
		}
	}

	event_base_dispatch(gw->base);
	exit(FPM_EXIT_OK);
}
/* }}} */

/* Listens on http_address when given, otherwise on the FastCGI address with the port bumped by
 * one. Returns -1 when that is not possible. */
static int fpm_http_listen(const char *pool, const char *listen_address, const char *http_address, int backlog, int reuseport) /* {{{ */
{
	char *dup_address = strdup(http_address ? http_address : listen_address), *host = NULL, *port_str = strrchr(dup_address, ':');
	char port[sizeof("65535")];
	struct addrinfo hints, *res, *p;
	int fd = -1, port_no, on = 1;

	if (port_str) {
		*port_str++ = '\0';
		host = dup_address;
		if (host[0] == '[' && host[strlen(host) - 1] == ']') {
			host[strlen(host) - 1] = '\0';
			host++;
		}
	} else {
		port_str = dup_address;
	}
	port_no = atoi(port_str) + (http_address ? 0 : 1);
	if (port_no < 1 || port_no > 65535) {
		zlog(ZLOG_WARNING, "[pool %s] no HTTP listener: no port left above '%s'", pool, listen_address);
		free(dup_address);
		return -1;
	}
	snprintf(port, sizeof(port), "%d", port_no);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if (getaddrinfo(host, port, &hints, &res) != 0) {
		zlog(ZLOG_WARNING, "[pool %s] no HTTP listener: cannot resolve '%s'", pool, listen_address);
		free(dup_address);
		return -1;
	}
	for (p = res; p && fd < 0; p = p->ai_next) {
		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd < 0) {
			continue;
		}
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
#ifdef SO_REUSEPORT
		if (reuseport) {
			setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
		}
#endif
		if (bind(fd, p->ai_addr, p->ai_addrlen) != 0 || listen(fd, backlog) != 0) {
			zlog(ZLOG_WARNING, "[pool %s] no HTTP listener: unable to listen on %s:%s: %s", pool, host ? host : "*", port, strerror(errno));
			close(fd);
			fd = -1;
			break;
		}
		fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
	}
	freeaddrinfo(res);
	free(dup_address);
	return fd;
}
/* }}} */

static void fpm_http_gateway_on_exit(void *arg, pid_t old_pid, int status);

static void fpm_http_gateway_spawn(struct fpm_http_gateway_s *gw, unsigned index) /* {{{ */
{
	gw->pids[index] = fork();
	if (gw->pids[index] < 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] http: fork() failed", gw->pool);
	} else if (gw->pids[index] == 0) {
		fpm_http_gateway_run(gw, index);
		/* not reached */
	} else {
		fpm_children_extra_watch(gw->pids[index], fpm_http_gateway_on_exit, gw->slots[index]);
	}
}
/* }}} */

/* Called by fpm_children_bury() (via fpm_children_extra_handle_exit()) when a
 * gateway process dies, however it dies — crash, OOM kill, whatever. Never
 * called for a deliberate shutdown: fpm_http_cleanup() forgets the pid first.
 * "Master respawns it like any other child" (docs/NOTES.md) without teaching
 * fpm_children.c anything about gateways — see fpm_children_extra.h. */
static void fpm_http_gateway_on_exit(void *arg, pid_t old_pid, int status) /* {{{ */
{
	struct fpm_http_gw_slot_s *slot = arg;
	struct fpm_http_gateway_s *gw = slot->gw;
	time_t now = time(NULL);

	if (slot->respawn.gave_up) {
		return; /* already logged once below, do not spam on every further death */
	}

	if (now - slot->respawn.window_start > FPM_HTTP_RESPAWN_WINDOW_SEC) {
		slot->respawn.window_start = now;
		slot->respawn.count = 0;
	}
	slot->respawn.count++;

	if (WIFSIGNALED(status)) {
		zlog(ZLOG_WARNING, "[pool %s] http gateway %u (pid %d) killed by signal %d, respawning",
			gw->pool, slot->index, (int) old_pid, WTERMSIG(status));
	} else {
		zlog(ZLOG_WARNING, "[pool %s] http gateway %u (pid %d) exited with code %d, respawning",
			gw->pool, slot->index, (int) old_pid, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	}

	if (!fpm_pctl_can_spawn_children()) {
		/* master is stopping/reloading: fpm_http_cleanup() is about to run
		 * (or already did) and will forget the survivors; nothing to spawn */
		gw->pids[slot->index] = 0;
		return;
	}

	if (slot->respawn.count > FPM_HTTP_RESPAWN_MAX_BURST) {
		slot->respawn.gave_up = 1;
		gw->pids[slot->index] = 0;
		zlog(ZLOG_ALERT, "[pool %s] http gateway %u crashed %u times within %d seconds, giving up on it "
			"(reload to try again); the pool now has one fewer gateway",
			gw->pool, slot->index, slot->respawn.count, FPM_HTTP_RESPAWN_WINDOW_SEC);
		return;
	}

	fpm_http_gateway_spawn(gw, slot->index);
}
/* }}} */

static void fpm_http_cleanup(int which, void *arg) /* {{{ */
{
	struct fpm_http_gateway_s *gw, *next;
	unsigned i;

	for (gw = gateways; gw; gw = next) {
		next = gw->next;
		for (i = 0; i < gw->nproc; i++) {
			if (gw->pids[i] > 0) {
				/* forget it BEFORE signalling it: this is a deliberate kill,
				 * not a crash, so fpm_children_extra_handle_exit() must not
				 * respawn it when the master's SIGCHLD handler reaps it */
				fpm_children_extra_forget(gw->pids[i]);
				kill(gw->pids[i], SIGTERM);
			}
		}
		for (i = 0; i < gw->nproc; i++) {
			if (gw->pids[i] > 0) {
				waitpid(gw->pids[i], NULL, 0);
			}
		}
		if (gw->listen_fd >= 0) {
			close(gw->listen_fd);
		}
		if (gw->plain_listen_fd >= 0) {
			close(gw->plain_listen_fd);
		}
		if (gw->upstreams_used) {
			fpm_shm_free((void*)gw->upstreams_used, sizeof(*gw->upstreams_used));
		}
#ifdef HAVE_FPM_HTTP_TLS
		if (gw->reload) {
			fpm_http_tls_reload_free(gw->reload);
		}
#endif
		for (i = 0; i < gw->nproc; i++) {
			free(gw->slots[i]);
		}
		free(gw->slots);
		free(gw->pids);
		fpm_http_acl_free(gw->acl);
		free(gw->allowed_clients);
		fpm_http_acl_free(gw->trusted_proxies_acl);
		free(gw->trusted_proxies);
		free(gw->front_controller);
		free(gw->access_log_path);
		free(gw->http_listen_override);
		free(gw->plain_listen_address);
		free(gw->pool);
		free(gw->listen_address);
		free(gw->docroot);
		free(gw);
	}
	gateways = NULL;
}
/* }}} */

static int cleanup_registered = 0;

/* The directive takes precedence when actually set
 * (fpm_conf_directive_was_set — the value alone cannot distinguish "unset" from
 * "set to the default"); env remains a fallback for deployments that already
 * use it. http.allowed_clients is a new directive and deliberately has no
 * environment fallback. */
static void fpm_http_gateway_settings(struct fpm_worker_pool_s *wp, struct fpm_http_gateway_s *gw, unsigned *nproc_wanted, int *reuseport_out) /* {{{ */
{
	const char *env;
	int idle_ms;

	if (fpm_conf_directive_was_set(wp->config, "http.gateways") && wp->config->http_gateways > 0) {
		*nproc_wanted = (unsigned) wp->config->http_gateways;
	} else if ((env = getenv("FPM_HTTP_GATEWAYS")) && atoi(env) > 0) {
		*nproc_wanted = (unsigned) atoi(env);
	} else {
		*nproc_wanted = wp->config->http_gateways > 0 ? (unsigned) wp->config->http_gateways : FPM_HTTP_GATEWAYS_DEFAULT;
	}

	if (fpm_conf_directive_was_set(wp->config, "http.reuseport")) {
		*reuseport_out = wp->config->http_reuseport;
	} else {
		env = getenv("FPM_HTTP_REUSEPORT");
		*reuseport_out = env && atoi(env) > 0;
	}
	gw->reuseport = *reuseport_out;

	/* No environment fallback, on purpose -- see
	 * fpm_http_upstream_write_must_fail(). */
	gw->fault_write_at = wp->config->http_fault_upstream_write > 0
		? wp->config->http_fault_upstream_write : 0;

	if (fpm_conf_directive_was_set(wp->config, "http.static")) {
		gw->static_files = wp->config->http_static;
	} else if ((env = getenv("FPM_HTTP_STATIC"))) {
		gw->static_files = atoi(env) > 0;
	} else {
		gw->static_files = wp->config->http_static;
	}

	if (fpm_conf_directive_was_set(wp->config, "http.idle_timeout")) {
		idle_ms = wp->config->http_idle_timeout;
	} else if ((env = getenv("FPM_HTTP_IDLE_MS"))) {
		idle_ms = atoi(env);
	} else {
		idle_ms = wp->config->http_idle_timeout;
	}
	gw->idle_ms = idle_ms;
	gw->idle_timeout.tv_sec = idle_ms / 1000;
	gw->idle_timeout.tv_usec = (idle_ms % 1000) * 1000;

	gw->read_timeout_ms = wp->config->http_read_timeout;
	gw->read_timeout.tv_sec = wp->config->http_read_timeout / 1000;
	gw->read_timeout.tv_usec = (wp->config->http_read_timeout % 1000) * 1000;
	gw->max_body = wp->config->http_max_body;

	if (fpm_conf_directive_was_set(wp->config, "http.listen") && wp->config->http_listen && *wp->config->http_listen) {
		gw->http_listen_override = strdup(wp->config->http_listen);
	} else if ((env = getenv("FPM_HTTP_LISTEN")) && *env) {
		gw->http_listen_override = strdup(env);
	}
	if (wp->config->http_plain_listen && *wp->config->http_plain_listen) {
		gw->plain_listen_address = strdup(wp->config->http_plain_listen);
	}

	if (wp->config->http_allowed_clients && *wp->config->http_allowed_clients) {
		gw->allowed_clients = strdup(wp->config->http_allowed_clients);
	}

	if (wp->config->http_trusted_proxies && *wp->config->http_trusted_proxies) {
		gw->trusted_proxies = strdup(wp->config->http_trusted_proxies);
	}

	if (wp->config->http_access_log && *wp->config->http_access_log) {
		gw->access_log_path = strdup(wp->config->http_access_log);
	}

	/* Default is "/index.php" (see the struct field's init in fpm_conf.c), so an
	 * unset directive already arrives here non-empty; strdup("") when the pool
	 * explicitly blanked it out to disable the fallback. */
	gw->front_controller = strdup(wp->config->http_front_controller ? wp->config->http_front_controller : "");

	/* The gateway drops to the same identity as the pool's own workers once
	 * it no longer needs root, see fpm_http_gateway_drop_privileges(). */
	gw->drop_uid = (uid_t) wp->set_uid;
	gw->drop_gid = (gid_t) wp->set_gid;
	/* wp->set_user is only populated when 'user' was a numeric id (see
	 * fpm_unix_conf_wp() in fpm_unix.c); otherwise fall back to the name as
	 * configured, exactly like fpm_unix_init_child() does for workers. */
	if (wp->set_user) {
		gw->drop_user = strdup(wp->set_user);
	} else if (wp->config->user && *wp->config->user) {
		gw->drop_user = strdup(wp->config->user);
	}

#ifdef HAVE_FPM_HTTP_TLS
	/* fpm_http_validate_pool() already refused a bad/mismatched cert+key at
	 * config-validation time; this is the real load, in the master, BEFORE
	 * fpm_http_gateway_spawn() forks the first child -- see fpm_http_tls.h. */
	if (wp->config->http_tls_cert && *wp->config->http_tls_cert) {
		gw->tls = fpm_http_tls_load(gw->pool, wp->config->http_tls_cert,
			wp->config->http_tls_key, wp->config->http_tls_min_version,
			wp->config->http_tls_sni_cert);
	}
	if (gw->tls) {
		/* http.tls_reload_check: unset -> a sensible non-zero default (task
		 * 040 exists precisely so a renewed certificate needs no operator
		 * action beyond the write); explicitly 0 -> off. Same
		 * was-it-set-at-all pattern as http.gateways above. */
		int interval = fpm_conf_directive_was_set(wp->config, "http.tls_reload_check")
			? wp->config->http_tls_reload_check
			: FPM_HTTP_TLS_RELOAD_CHECK_DEFAULT;

		if (interval < 0) {
			interval = 0;
		}
		gw->reload = fpm_http_tls_reload_master_init(gw->pool, wp->config->http_tls_cert,
			wp->config->http_tls_key, wp->config->http_tls_min_version, gw->tls, interval);
	}
#endif
}
/* }}} */

/* Called once per http pool by the master, before worker forks.
 * capacity_override is needed by multi-request executors: a classic worker
 * holds one connection, while a Fiber holds many. 0 preserves the child-count
 * limit. */
static int fpm_http_init_pool_ex(struct fpm_worker_pool_s *wp, unsigned capacity_override) /* {{{ */
{
	char cwd[MAXPATHLEN];

	if (!getcwd(cwd, sizeof(cwd))) {
		strcpy(cwd, "/");
	}

	{
		struct fpm_http_gateway_s *gw;
		unsigned workers = wp->config->pm_max_children > 0 ? (unsigned)wp->config->pm_max_children : 1;
		unsigned capacity = capacity_override ? capacity_override : workers;
		const char *capacity_env = capacity_override ? getenv("FPM_HTTP_MAX_UPSTREAMS") : NULL;
		unsigned nproc_wanted;
		int reuseport;
		unsigned i;

		if (capacity_env && atoi(capacity_env) > 0) {
			capacity = (unsigned) atoi(capacity_env);
		}

		gw = calloc(1, sizeof(*gw));
		gw->listen_fd = -1;
		gw->plain_listen_fd = -1;
		gw->pool = strdup(wp->config->name);
		gw->listen_address = strdup(wp->config->listen_address);
		gw->docroot = strdup(wp->config->chdir && *wp->config->chdir ? wp->config->chdir : cwd);
		gw->backlog = wp->config->listen_backlog;
		fpm_http_gateway_settings(wp, gw, &nproc_wanted, &reuseport);
		/* Needs gw->docroot and gw->front_controller, both set above; runs once
		 * here in the master so every forked gateway process inherits the
		 * verdict instead of each evaluating it on its own first request. */
		fpm_http_front_controller_validate(gw);

#ifdef HAVE_FPM_HTTP_TLS
		/* fpm_http_tls_load() already logged what went wrong; http.tls_cert
		 * was set, so falling back to plain HTTP would be a silent surprise. */
		if (wp->config->http_tls_cert && *wp->config->http_tls_cert && !gw->tls) {
			free(gw->allowed_clients);
			free(gw->trusted_proxies);
			free(gw->access_log_path);
			free(gw->http_listen_override);
			free(gw->plain_listen_address);
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return -1;
		}
#endif

		/* a UNIX socket pool has no port to bump, so it needs an explicit HTTP
		 * address — fpm_http_validate_pool() already refused to start without
		 * one; this is just a defensive fallback, unreachable in practice */
		if (wp->listen_address_domain != FPM_AF_INET && !gw->http_listen_override) {
			free(gw->allowed_clients);
			free(gw->trusted_proxies);
			free(gw->front_controller);
			free(gw->access_log_path);
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return 0;
		}

		if (gw->allowed_clients && fpm_http_acl_parse(gw->pool, "http.allowed_clients", gw->allowed_clients, &gw->acl) != 0) {
			free(gw->allowed_clients);
			free(gw->trusted_proxies);
			free(gw->front_controller);
			free(gw->access_log_path);
			free(gw->http_listen_override);
			free(gw->plain_listen_address);
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return -1;
		}

		if (gw->trusted_proxies && fpm_http_acl_parse(gw->pool, "http.trusted_proxies", gw->trusted_proxies, &gw->trusted_proxies_acl) != 0) {
			fpm_http_acl_free(gw->acl);
			free(gw->allowed_clients);
			free(gw->trusted_proxies);
			free(gw->front_controller);
			free(gw->access_log_path);
			free(gw->http_listen_override);
			free(gw->plain_listen_address);
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return -1;
		}

		gw->listen_fd = fpm_http_listen(gw->pool, gw->listen_address, gw->http_listen_override, gw->backlog, reuseport);
		if (gw->listen_fd < 0) {
			fpm_http_acl_free(gw->acl);
			free(gw->allowed_clients);
			fpm_http_acl_free(gw->trusted_proxies_acl);
			free(gw->trusted_proxies);
			free(gw->front_controller);
			free(gw->access_log_path);
			free(gw->http_listen_override);
			free(gw->plain_listen_address);
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return 0;
		}
		if (gw->plain_listen_address) {
			gw->plain_listen_fd = fpm_http_listen(gw->pool, gw->listen_address, gw->plain_listen_address, gw->backlog, reuseport);
			if (gw->plain_listen_fd < 0) {
				close(gw->listen_fd);
				fpm_http_acl_free(gw->acl);
				free(gw->allowed_clients);
				fpm_http_acl_free(gw->trusted_proxies_acl);
				free(gw->trusted_proxies);
				free(gw->front_controller);
				free(gw->access_log_path);
				free(gw->http_listen_override);
				free(gw->plain_listen_address);
				free(gw->pool);
				free(gw->listen_address);
				free(gw->docroot);
				free(gw);
				return 0;
			}
		}
		/* A classic worker handles one connection at a time; a multi-request
		 * executor supplies its own capacity independently of the child count. */
		gw->nproc = MIN(nproc_wanted, workers);
		gw->max_upstreams = capacity;
		gw->upstreams_used = fpm_shm_alloc(sizeof(*gw->upstreams_used));
		if (!gw->upstreams_used) {
			zlog(ZLOG_ERROR, "[pool %s] http: cannot allocate shared memory", wp->config->name);
			close(gw->listen_fd);
			fpm_http_acl_free(gw->acl);
			free(gw->allowed_clients);
			fpm_http_acl_free(gw->trusted_proxies_acl);
			free(gw->trusted_proxies);
			free(gw->front_controller);
			free(gw->access_log_path);
			free(gw->http_listen_override);
			free(gw->plain_listen_address);
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return -1;
		}
		*gw->upstreams_used = 0;
		gw->pids = calloc(gw->nproc, sizeof(pid_t));
		/* array of pointers — sizeof(void *) is intentional */
		gw->slots = calloc(gw->nproc, sizeof(void *));
		gw->next = gateways;
		gateways = gw;
		zlog(ZLOG_NOTICE, "[pool %s] HTTP listener: %u gateway(s)%s%s, %u persistent connection(s) to the pool",
			wp->config->name, gw->nproc, reuseport ? " with SO_REUSEPORT" : "",
			gw->acl ? ", access-restricted" : "", capacity);

		for (i = 0; i < gw->nproc; i++) {
			gw->slots[i] = calloc(1, sizeof(*gw->slots[i]));
			gw->slots[i]->gw = gw;
			gw->slots[i]->index = i;
			gw->slots[i]->respawn.window_start = time(NULL);
			fpm_http_gateway_spawn(gw, i);
		}
		if (reuseport) {
			/* the master's socket would otherwise take its share of connections and never accept them */
			close(gw->listen_fd);
			gw->listen_fd = -1;
			if (gw->plain_listen_fd >= 0) {
				close(gw->plain_listen_fd);
				gw->plain_listen_fd = -1;
			}
		}
	}

	/* Register cleanup once, when the first http pool is initialized.
	 * PARENT_EXEC too, because reload calls execvp() and without this the
	 * gateways would remain orphaned while holding the port on which the new
	 * master wants to bind. */
	if (!cleanup_registered) {
		if (0 > fpm_cleanup_add(FPM_CLEANUP_PARENT, fpm_http_cleanup, 0) ||
		    0 > fpm_cleanup_add(FPM_CLEANUP_PARENT_EXEC, fpm_http_cleanup, 0)) {
			return -1;
		}
		cleanup_registered = 1;
	}
	return 0;
}
/* }}} */

/* Checks specific to pool.type = http, called by fpm_pool_type.c while
 * validating the configuration, before anything forks. */
int fpm_http_validate_pool(struct fpm_worker_pool_s *wp) /* {{{ */
{
	if (fpm_conf_directive_was_set(wp->config, "http.gateways") && wp->config->http_gateways < 1) {
		zlog(ZLOG_ERROR, "[pool %s] http.gateways must be at least 1", wp->config->name);
		return -1;
	}
	if (fpm_conf_directive_was_set(wp->config, "http.idle_timeout") && wp->config->http_idle_timeout < 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.idle_timeout must not be negative", wp->config->name);
		return -1;
	}
	if (wp->config->http_read_timeout < 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.read_timeout must not be negative", wp->config->name);
		return -1;
	}
	if (wp->listen_address_domain != FPM_AF_INET) {
		int has_directive = fpm_conf_directive_was_set(wp->config, "http.listen")
			&& wp->config->http_listen && *wp->config->http_listen;
		const char *env = getenv("FPM_HTTP_LISTEN");

		if (!has_directive && !(env && *env)) {
			zlog(ZLOG_ERROR, "[pool %s] pool.type = http requires http.listen when listen is a unix socket "
				"(there is no FastCGI port to bump by one)", wp->config->name);
			return -1;
		}
	}
	if (wp->config->http_allowed_clients && *wp->config->http_allowed_clients) {
		struct fpm_http_acl_s *tmp = NULL;

		if (fpm_http_acl_parse(wp->config->name, "http.allowed_clients", wp->config->http_allowed_clients, &tmp) != 0) {
			return -1; /* fpm_http_acl_parse() already logged which address is bad */
		}
		fpm_http_acl_free(tmp);
	}
	if (wp->config->http_trusted_proxies && *wp->config->http_trusted_proxies) {
		struct fpm_http_acl_s *tmp = NULL;

		if (fpm_http_acl_parse(wp->config->name, "http.trusted_proxies", wp->config->http_trusted_proxies, &tmp) != 0) {
			return -1; /* fpm_http_acl_parse() already logged which address is bad */
		}
		fpm_http_acl_free(tmp);
	}
	if (wp->config->http_front_controller && *wp->config->http_front_controller) {
		const char *fc = wp->config->http_front_controller;
		size_t len = strlen(fc);

		if (fc[0] != '/' || strstr(fc, "/../") || (len >= 3 && !strcmp(fc + len - 3, "/.."))) {
			zlog(ZLOG_ERROR, "[pool %s] http.front_controller must be an absolute path under the document root, without '..'", wp->config->name);
			return -1;
		}
	}
	if (wp->config->http_plain_listen && *wp->config->http_plain_listen &&
			(!wp->config->http_tls_cert || !*wp->config->http_tls_cert)) {
		zlog(ZLOG_ERROR, "[pool %s] http.plain_listen requires http.tls_cert", wp->config->name);
		return -1;
	}
	if (wp->config->http_tls_cert && *wp->config->http_tls_cert) {
#ifdef HAVE_FPM_HTTP_TLS
		if (!wp->config->http_tls_key || !*wp->config->http_tls_key) {
			zlog(ZLOG_ERROR, "[pool %s] http.tls_cert requires http.tls_key", wp->config->name);
			return -1;
		}
		/* Reads cert+key from disk into a throwaway SSL_CTX and checks they
		 * parse and match -- a bad path or a mismatched key must fail here,
		 * before fpm_http_init_pool_ex() forks a single gateway child, not
		 * as a crash or a silent plain-HTTP fallback at request time. */
		if (fpm_http_tls_validate(wp->config->name, wp->config->http_tls_cert, wp->config->http_tls_key,
				wp->config->http_tls_min_version, wp->config->http_tls_sni_cert) != 0) {
			return -1; /* fpm_http_tls_validate() already logged what is wrong */
		}
#else
		zlog(ZLOG_ERROR, "[pool %s] http.tls_cert requires the HTTP gateway to be built with TLS support "
			"(libevent_openssl and/or OpenSSL were not found at build time)", wp->config->name);
		return -1;
#endif
	} else if (wp->config->http_tls_key && *wp->config->http_tls_key) {
		zlog(ZLOG_ERROR, "[pool %s] http.tls_key without http.tls_cert has nothing to attach the key to", wp->config->name);
		return -1;
	}
	return 0;
}
/* }}} */

int fpm_http_init_pool(struct fpm_worker_pool_s *wp) /* {{{ */
{
	return fpm_http_init_pool_ex(wp, 0);
}
/* }}} */

int fpm_http_init_pool_with_capacity(struct fpm_worker_pool_s *wp, unsigned capacity) /* {{{ */
{
	return fpm_http_init_pool_ex(wp, capacity);
}
/* }}} */

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

#endif
