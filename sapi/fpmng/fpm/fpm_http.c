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
#include <ctype.h>
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
#include "fpm_children_extra.h"
#include "fpm_http_tls.h"
#include "fpm_http_tls_reload.h"
#include "zlog.h"

#define FPM_HTTP_GATEWAYS_DEFAULT 2			/* http.gateways default; also the FPM_HTTP_GATEWAYS env fallback */
#define FPM_HTTP_IDLE_MS         500		/* http.idle_timeout default (ms); release a pinned worker after this much idle time */
/* A crash loop (bad bind, OOM, ...) must not turn into an unbounded fork()
 * storm: after this many respawns within RESPAWN_WINDOW seconds, a gateway
 * slot gives up and stays dead until the next reload. */
#define FPM_HTTP_RESPAWN_MAX_BURST 5
#define FPM_HTTP_RESPAWN_WINDOW_SEC 10
#define FPM_HTTP_MAX_BODY        (32 * 1024 * 1024)
#define FPM_HTTP_MAX_CGI_HEADERS (64 * 1024)
#define FCGI_MAX_RECORD_LEN      0xffff
#define FPM_HTTP_BAD_GATEWAY     502 /* libevent has no constant for it */

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
	int backlog;
	int reuseport;					/* every gateway binds its own SO_REUSEPORT socket (http.reuseport) */
	unsigned nproc;
	pid_t *pids;
	struct fpm_http_gw_slot_s **slots;		/* one per pids[i], see fpm_http_gw_slot_s */
	int static_files;				/* http.static, per pool: fork() copies it into every gateway process */
	int idle_ms;					/* http.idle_timeout, milliseconds; 0 = never drop an idle pinned connection */
	struct timeval idle_timeout;			/* idle_ms split into {sec, usec} for event_add() */
	char *allowed_clients;				/* http.allowed_clients, raw string kept for fpm_http_acl_parse() */
	struct fpm_http_acl_s *acl;			/* NULL = no restriction, see fpm_http_acl.h */
	char *http_listen_override;			/* http.listen; NULL = derive from listen_address (port + 1) */
	char *trusted_proxies;				/* http.trusted_proxies, raw string kept for fpm_http_acl_parse() */
	struct fpm_http_acl_s *trusted_proxies_acl;	/* NULL = nikomu nie ufamy, see fpm_http_forwarded.h */
	char *access_log_path;				/* http.access_log; NULL = wylaczony */
	struct fpm_http_access_log_s *access_log;	/* gateway process only, NULL w masterze */
	char *front_controller;			/* http.front_controller; puste = fallback wylaczony (dzisiejsze zachowanie) */

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
	/* http.tls_cert/http.tls_key; NULL = zwykly HTTP, dokladnie jak dzis.
	 * gw->tls jest wczytany DO PAMIECI w masterze, PRZED forkiem pierwszego
	 * dziecka (fpm_http_tls_load()) -- fork() go kopiuje. gw->tls_ctx jest
	 * per-proces: kazde dziecko buduje WLASNY SSL_CTX z tych samych bajtow
	 * (fpm_http_tls_ctx_new(), wolane z fpm_http_gateway_run()), zeby
	 * wspolny klucz ticketow w gw->tls dzialal dla wznowienia sesji miedzy
	 * procesami, patrz fpm_http_tls.h. */
	struct fpm_http_tls_s *tls;			/* NULL w dziecku po nieudanym starcie */
	SSL_CTX *tls_ctx;				/* tylko w dziecku, NULL w masterze */
	/* NULL gdy http.tls_reload_check = 0 albo alokacja shm sie nie udala --
	 * bramka wtedy dziala dokladnie tak jak przed tym taskiem (task 040). */
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
};

static struct fpm_http_gateway_s *gateways = NULL;

/* one HTTP request being proxied */
struct _fpm_http_conn {
	struct fpm_http_gateway_s *gw;
	struct evhttp_request *req;
	struct evhttp_connection *evcon;
	fpm_http_upstream *upstream;		/* while in flight */
	int queued;
	TAILQ_ENTRY(_fpm_http_conn) link;

	smart_str params;					/* FCGI_PARAMS payload being assembled */
	smart_str out;						/* records ready to go upstream */

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

	/* FastCGI record stream from the pool */
	unsigned char rec_hdr[8];
	int rec_hdr_len, rec_type, rec_len, rec_pad;
};

static void fpm_http_pump(struct fpm_http_gateway_s *gw);
static void fpm_http_retry_later(struct fpm_http_gateway_s *gw);

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
static int fpm_http_front_controller_ok(struct fpm_http_gateway_s *gw);

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
 * script_missing_hint is fpm_http_try_local()'s realpath() verdict on the exact
 * path this function would otherwise stat() itself (0 = exists, 1 = confirmed
 * missing, -1 = not checked there) -- see the comment on fpm_http_serve_static(). */
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
	 * this request maps to does not exist, hand it to the front controller instead
	 * and let PATH_INFO carry the original path -- same idea as php -S's own
	 * fallback to index.php (php_cli_server_request_translate_vpath()), minus its
	 * walk-left-and-stat() loop, which is fine for a dev server but too many
	 * syscalls per request for here.
	 *
	 * Cost: when path_info is NULL and there was no trailing slash (the plain
	 * "/mix" case, no .php split or appended index.php), fpm_http_try_local()
	 * already ran this exact realpath() for GET/HEAD with http.static on, so
	 * script_missing_hint answers it for free. Everywhere else -- POST/PUT/...,
	 * http.static = 0, a request for a bare .php file (fpm_http_serve_static()
	 * steps aside for those on purpose), or a trailing-slash/.php-split path --
	 * this is the one extra stat() the fallback adds, and only when
	 * http.front_controller is non-empty in the first place. */
	if (fpm_http_front_controller_ok(c->gw)) {
		int missing;

		if (!path_info && !trailing_slash && script_missing_hint >= 0) {
			missing = script_missing_hint;
		} else {
			struct stat st;

			missing = (stat(ZSTR_VAL(filename.s), &st) != 0);
		}
		if (missing) {
			smart_str_free(&filename);
			smart_str_appends(&filename, c->gw->docroot);
			smart_str_appends(&filename, c->gw->front_controller);
			smart_str_0(&filename);
			path_info = decoded;	/* whole original path, whatever split/index.php rule ran above */
		}
	}

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

		if (strcasecmp(k, "Content-Length") == 0) {
			continue;
		}
		if (strcasecmp(k, "Content-Type") != 0) {
			smart_str_appendl(&name, "HTTP_", sizeof("HTTP_") - 1);
		}
		for (; *k; k++) {
			smart_str_appendc(&name, *k == '-' ? '_' : toupper((unsigned char)*k));
		}
		smart_str_0(&name);
		fpm_http_param(c, ZSTR_VAL(name.s), header->value);
		smart_str_free(&name);
	}

	/* BEGIN_REQUEST, PARAMS (possibly several records), empty PARAMS, STDIN, empty STDIN */
	fpm_http_fcgi_record(&c->out, FCGI_BEGIN_REQUEST, begin_request, sizeof(begin_request));
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

/* The pool is done with the request (END_REQUEST seen or the connection failed). */
static void fpm_http_finish(fpm_http_conn *c, int upstream_ok)
{
	if (c->headers_sent) {
		evhttp_send_reply_end(c->req);
	} else if (c->cgi_headers.s) {
		fpm_http_start_reply(c, 0, 0); /* partial header block, ship what we have */
		evhttp_send_reply_end(c->req);
	} else {
		if (!upstream_ok) {
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

static void fpm_http_upstream_drop(fpm_http_upstream *up)
{
	struct fpm_http_gateway_s *gw = up->gw;

	TAILQ_REMOVE(&gw->upstreams, up, link);
	gw->nupstreams--;
	if (up->ev_read) {
		event_free(up->ev_read);
	}
	if (up->ev_write) {
		event_free(up->ev_write);
	}
	close(up->fd);
	smart_str_free(&up->pending);
	free(up);
	fpm_http_budget_give_back(gw);
}

/* the pool went away mid-request or while idle */
static void fpm_http_upstream_fail(fpm_http_upstream *up, int clean_eof)
{
	struct fpm_http_gateway_s *gw = up->gw;

	if (!clean_eof) {
		zlog(ZLOG_WARNING, "[pool %s] http: upstream '%s': %s", gw->pool, gw->listen_address, strerror(errno));
	}
	/* EOF is normal after pm.max_requests or a worker restart; a request in flight is lost though */
	if (up->current) {
		fpm_http_finish(up->current, clean_eof);
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
		}
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

/* Writes whatever is pending; registers the write event only when the socket is full. */
static void fpm_http_upstream_flush(fpm_http_upstream *up)
{
	while (up->pending.s && up->pending_off < ZSTR_LEN(up->pending.s)) {
		ssize_t n = write(up->fd, ZSTR_VAL(up->pending.s) + up->pending_off, ZSTR_LEN(up->pending.s) - up->pending_off);

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

/* Hands waiting requests to free connections, opening new ones up to this process' share. */
static void fpm_http_pump(struct fpm_http_gateway_s *gw)
{
	while (!TAILQ_EMPTY(&gw->waiting)) {
		fpm_http_upstream *up, *idle = NULL;
		fpm_http_conn *c;

		TAILQ_FOREACH(up, &gw->upstreams, link) {
			if (!up->busy) {
				idle = up;
				break;
			}
		}
		if (!idle) {
			idle = fpm_http_upstream_new(gw);
		}
		if (!idle && gw->nupstreams == 0) {
			/* no connection of our own and no budget left: another gateway holds every worker,
			 * so look again shortly instead of waiting for an END_REQUEST that cannot come */
			fpm_http_retry_later(gw);
			return;
		}
		if (!idle) {
			return; /* everything busy, the next END_REQUEST calls us again */
		}

		c = TAILQ_FIRST(&gw->waiting);
		TAILQ_REMOVE(&gw->waiting, c, link);
		c->queued = 0;
		c->upstream = idle;
		idle->busy = 1;
		idle->current = c;
		if (gw->idle_ms > 0 && !idle->connecting) {
			event_add(idle->ev_read, NULL);		/* drop the idle deadline for the duration of the request */
		}
		fpm_http_upstream_write(idle, ZSTR_VAL(c->out.s), ZSTR_LEN(c->out.s));
		smart_str_free(&c->out);
	}
}

static void fpm_http_retry_cb(evutil_socket_t fd, short what, void *arg)
{
	fpm_http_pump(arg);
}

static void fpm_http_retry_later(struct fpm_http_gateway_s *gw)
{
	static const struct timeval retry = {0, 2000};

	event_base_once(gw->base, -1, EV_TIMEOUT, fpm_http_retry_cb, gw, &retry);
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
 * Odpowiedzi, ktore bramka daje SAMA, bez zajmowania workera.
 *
 * To jest jeden punkt na wszystkie takie przypadki. Dzis pliki statyczne;
 * pozniej doloza sie tu wyzwanie ACME (/.well-known/acme-challenge/) i
 * /status. Nie robic z tego doraznych if-ow w fpm_http_request — kazdy z tych
 * przypadkow potrzebuje dokladnie tego samego: rozwiazac sciezke, sprawdzic
 * zawieranie w katalogu, odpowiedziec bez FastCGI.
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

/* Validates http.front_controller once per gateway process (empty option ->
 * fallback disabled, same as today). The value is admin config, not request
 * input, but it still goes through the same realpath()-under-docroot check as
 * a static file (see fpm_http_serve_static): a symlink can put a perfectly
 * innocent-looking path outside the document root, and there is no reason to
 * trust config more than we trust the filesystem. When the front controller
 * is not deployed yet, realpath() has nothing to resolve -- the fallback is
 * still enabled in that case, so a request that reaches it gets the worker's
 * usual "File not found" instead of silently behaving as if the option were
 * unset. */
static int fpm_http_front_controller_ok(struct fpm_http_gateway_s *gw)
{
	static int state = 0; /* 0 = not checked yet, 1 = usable, -1 = disabled/invalid */

	if (!state) {
		const char *fc = gw->front_controller;

		state = -1;
		if (fc && *fc) {
			const char *root = fpm_http_docroot_real(gw);
			char candidate[MAXPATHLEN], resolved[MAXPATHLEN];

			if (!root) {
				zlog(ZLOG_WARNING, "[pool %s] http: http.front_controller fallback disabled, document root does not resolve", gw->pool);
			} else if ((size_t)snprintf(candidate, sizeof(candidate), "%s%s", gw->docroot, fc) >= sizeof(candidate)) {
				zlog(ZLOG_WARNING, "[pool %s] http: http.front_controller '%s' is too long, fallback disabled", gw->pool, fc);
			} else if (!realpath(candidate, resolved)) {
				state = 1;	/* not deployed yet: still enable, see the comment above */
			} else {
				size_t root_len = strlen(root);

				if (!strncmp(resolved, root, root_len) && (!resolved[root_len] || resolved[root_len] == '/')) {
					state = 1;
				} else {
					zlog(ZLOG_WARNING, "[pool %s] http: http.front_controller '%s' resolves outside the document root, fallback disabled", gw->pool, fc);
				}
			}
		}
	}

	return state == 1;
}

/* cmd == EVHTTP_REQ_GET or EVHTTP_REQ_HEAD, and http.static is on: fills in
 * *script_missing with what realpath() below finds out about the same path
 * fpm_http_build_request() would use as SCRIPT_FILENAME (0 = exists, 1 =
 * confirmed missing), so that build_request can skip its own stat() for the
 * one case this function already paid for. -1 (unchanged from the caller's
 * initial value) means this function never reached that check -- the caller
 * still doesn't know. */
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
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return 0;			/* directories and specials go to the worker */
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

	/* Kolejnosc bedzie miala znaczenie, gdy dojda ACME i /status: najpierw
	 * rzeczy o ustalonej sciezce, dopiero na koncu pliki z dysku. */
	answered = fpm_http_serve_static(gw, req, path, path_len, remote_addr, script_missing);

	free(path);

	return answered;
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

	/* Odpowiedzi lokalne najpierw: nie ma sensu budowac parametrow FastCGI ani
	 * zajmowac slotu workera dla pliku, ktory oddamy sami. -1 = "nie sprawdzano"
	 * (nie GET/HEAD, albo http.static = 0): fpm_http_build_request() wtedy sam
	 * zdecyduje, czy potrzebuje wlasnego stat() dla http.front_controller. */
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
	}

	/* Everything above this line is the only reason the gateway ever needed
	 * root: binding http.reuseport's own listener, and holding the TLS
	 * private key the master read before the first fork (fpm_http_tls.h).
	 * Nothing below -- the access log, static files, TLS handshakes, proxying
	 * to the pool -- needs it. See tasks/010-http-gateway-drop-privileges.md. */
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
		 * master already read and validated (fpm_http_tls_load()), never
		 * from an SSL_CTX inherited through fork() -- see fpm_http_tls.h. */
		gw->tls_ctx = fpm_http_tls_ctx_new(gw->pool, gw->tls);
		if (!gw->tls_ctx) {
			exit(FPM_EXIT_SOFTWARE);
		}
		evhttp_set_bevcb(gw->http, fpm_http_tls_bevcb, gw->tls_ctx);

		/* Own generation-watch timer, on this child's own base -- see
		 * fpm_http_tls_reload.h. No-op when gw->reload is NULL. */
		fpm_http_tls_reload_child_init(gw->reload, gw->base, gw->http, &gw->tls_ctx);
	}
#endif
	evhttp_set_allowed_methods(gw->http, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD | EVHTTP_REQ_PUT |
		EVHTTP_REQ_DELETE | EVHTTP_REQ_OPTIONS | EVHTTP_REQ_PATCH);
	evhttp_set_max_body_size(gw->http, FPM_HTTP_MAX_BODY);
	evhttp_set_gencb(gw->http, fpm_http_request, gw);
	evutil_make_socket_nonblocking(gw->listen_fd);
	if (evhttp_accept_socket(gw->http, gw->listen_fd) != 0) {
		zlog(ZLOG_ERROR, "[pool %s] http: evhttp_accept_socket() failed", gw->pool);
		exit(FPM_EXIT_SOFTWARE);
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
		free(gw->pool);
		free(gw->listen_address);
		free(gw->docroot);
		free(gw);
	}
	gateways = NULL;
}
/* }}} */

static int cleanup_registered = 0;

/* Dyrektywa ma pierwszenstwo, gdy faktycznie ustawiona (fpm_conf_directive_was_set —
 * z samej wartosci nie da sie odroznic "nieustawione" od "ustawione na domyslna");
 * env zostaje jako fallback dla wdrozen, ktore go juz uzywaja. http.allowed_clients
 * jest nowa dyrektywa i celowo bez fallbacku envowego. */
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

	if (fpm_conf_directive_was_set(wp->config, "http.listen") && wp->config->http_listen && *wp->config->http_listen) {
		gw->http_listen_override = strdup(wp->config->http_listen);
	} else if ((env = getenv("FPM_HTTP_LISTEN")) && *env) {
		gw->http_listen_override = strdup(env);
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

/* Wolane raz na pool typu http, ze strony mastera, przed forkiem workerow.
 * capacity_override jest potrzebne executorom wielorequestowym: klasyczny
 * worker trzyma jedno polaczenie, Fiber wiele. 0 zachowuje limit liczby dzieci. */
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
		gw->pool = strdup(wp->config->name);
		gw->listen_address = strdup(wp->config->listen_address);
		gw->docroot = strdup(wp->config->chdir && *wp->config->chdir ? wp->config->chdir : cwd);
		gw->backlog = wp->config->listen_backlog;
		fpm_http_gateway_settings(wp, gw, &nproc_wanted, &reuseport);

#ifdef HAVE_FPM_HTTP_TLS
		/* fpm_http_tls_load() already logged what went wrong; http.tls_cert
		 * was set, so falling back to plain HTTP would be a silent surprise. */
		if (wp->config->http_tls_cert && *wp->config->http_tls_cert && !gw->tls) {
			free(gw->allowed_clients);
			free(gw->trusted_proxies);
			free(gw->access_log_path);
			free(gw->http_listen_override);
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
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return 0;
		}
		/* Klasyczny worker obsluguje jedno polaczenie naraz; executor
		 * wielorequestowy podaje wlasna pojemnosc niezalezna od liczby dzieci. */
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
		}
	}

	/* Sprzatanie rejestrujemy raz, przy pierwszym poolu http.
	 * PARENT_EXEC tez, bo reload robi execvp() i bez tego bramki zostalyby
	 * osierocone, trzymajac port, na ktorym nowy master chce sie zbindowac. */
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

/* Sprawdzenia specyficzne dla pool.type = http, wolane przez fpm_pool_type.c
 * podczas walidacji configu, przed forkiem czegokolwiek. */
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
