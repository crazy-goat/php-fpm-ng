/* fpm-ng: the gateway's FastCGI codec: record encoding, request -> FastCGI and FastCGI -> response.
 *
 * Split out of fpm_http.c (#747); a pure move. The shared definitions are in
 * fpm_http_internal.h. */
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
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
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
#include "fpm_http_direct_request.h"
#include "fpm_children_extra.h"
#include "fpm_pool_type.h"
#include "fpm_tls_http.h"
#include "fpm_tls_reload.h"
#include "fpm_http_static.h"
#include "fpm_child_error_log.h"
#include "fpm_error_log_follow.h"
#include "fpm_operator_http.h"
#include "fpm_operator_endpoint.h"
#include "zlog.h"

#include "fpm_http_internal.h"

/* ---------------------------------------------------------------- FastCGI encoding */

static void fpm_http_fcgi_record(smart_str *out, int type, const char *data, size_t len)
{
	unsigned char hdr[8] = { FCGI_VERSION_1, (unsigned char) type, 0, 1, (unsigned char) (len >> 8), (unsigned char) len, (unsigned char) ((8 - len % 8) % 8), 0 };
	static const char zeros[8] = { 0 };

	smart_str_appendl(out, (char *) hdr, sizeof(hdr));
	smart_str_appendl(out, data, len);
	smart_str_appendl(out, zeros, hdr[6]);
}

static void fpm_http_fcgi_len(smart_str *out, size_t len)
{
	if (len < 0x80) {
		smart_str_appendc(out, (char) len);
	} else {
		unsigned char b[4] = { (unsigned char) ((len >> 24) | 0x80), (unsigned char) (len >> 16), (unsigned char) (len >> 8), (unsigned char) len };
		smart_str_appendl(out, (char *) b, 4);
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

/* Skips an absolute-form scheme://authority and reports the authority (without
 * userinfo); a target without one is returned unchanged. Copies nothing. */
static void fpm_http_origin_start(const char *uri, const char **authority, size_t *authority_len)
{
	const char *p = uri;

	if (authority) {
		*authority = NULL;
		*authority_len = 0;
	}
	if (((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z')) {
		p++;
		while (((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z') || (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.') {
			p++;
		}
		if (p[0] == ':' && p[1] == '/' && p[2] == '/') {
			const char *auth = p + 3;

			p = auth;
			while (*p && *p != '/' && *p != '?' && *p != '#') {
				p++;
			}
			if (authority) {
				const char *at;

				/* userinfo is not part of a Host value (RFC 9110 7.2) */
				for (at = p; at > auth && at[-1] != '@'; at--) {
				}
				*authority = at;
				*authority_len = (size_t) (p - at);
			}
			return;
		}
	}
}

/* 1 = copied, 0 = no authority, -1 = authority longer than buf_len-1. */
int fpm_http_absolute_authority(const char *uri, char *buf, size_t buf_len)
{
	const char *authority;
	size_t len;

	if (!uri) {
		return 0;
	}
	fpm_http_origin_start(uri, &authority, &len);
	if (!authority || !len) {
		return 0;
	}
	if (len >= buf_len) {
		return -1; /* over-long: the caller answers 400 rather than fall back to Host */
	}
	memcpy(buf, authority, len);
	buf[len] = '\0';
	return 1;
}

/* Ingress step (#534), first thing in both listeners' request callbacks: a
 * target that starts with "/" is an origin-form absolute-path (RFC 9112 3.2.1),
 * so "//api/users" is the path "//api/users". libevent reads it as a
 * network-path reference (host "api", path "/users"); rewrite its parse so the
 * host is dropped and the path is the raw one up to "?" or "#". Only a target
 * with a scheme keeps libevent's reading of an authority. From here on every
 * consumer -- matchers, router, static lookup, ACME, REQUEST_URI -- sees one
 * path, so nothing is matched on one path and served on another. */
void fpm_http_normalize_target(struct evhttp_request *req)
{
	struct evhttp_uri *u = (struct evhttp_uri *) evhttp_request_get_evhttp_uri(req);
	const char *uri = evhttp_request_get_uri(req);
	size_t len;
	char *path;

	if (!u || !uri || uri[0] != '/' || uri[1] != '/') {
		return;
	}
	len = strcspn(uri, "?#");
	path = estrndup(uri, len);
	if (evhttp_uri_set_path(u, path) == 0) {
		evhttp_uri_set_host(u, NULL);
		evhttp_uri_set_port(u, -1);
		evhttp_uri_set_userinfo(u, NULL);
	}
	efree(path);
}

/* The path every consumer of the request-target uses (see
 * fpm_http_normalize_target()). "http:/metrics/app" yields "/metrics/app"; an
 * empty path is "/" only when an authority was present ("GET http://h",
 * RFC 9112 3.2.2). */
const char *fpm_http_request_path(struct evhttp_request *req)
{
	const struct evhttp_uri *u = evhttp_request_get_evhttp_uri(req);
	const char *p = u ? evhttp_uri_get_path(u) : NULL;

	if (u && p && !*p && evhttp_uri_get_host(u)) {
		return "/";
	}
	return p;
}

/* The path of the request as every gateway matcher (ping.path, the operator
 * namespace, access.suppress_path[]) compares it: fpm_http_request_path(), raw
 * (no percent-decoding), the query already cut off by libevent's parse. Returns
 * the length, or 0 when there is no path or it does not fit `path_size`-1
 * bytes (#534). */
size_t fpm_http_raw_path(struct evhttp_request *req, char *path, size_t path_size)
{
	const char *p = fpm_http_request_path(req);
	size_t len;

	if (!p) {
		return 0;
	}
	len = strlen(p);
	if (len == 0 || len >= path_size) {
		return 0;
	}
	memcpy(path, p, len);
	path[len] = '\0';
	return len;
}

/* The origin-form target (path and "?query") the gateway forwards: REQUEST_URI
 * on the FastCGI transport, the request line of an http.route[] target and
 * the 308 Location of the plain listener. Built from the same libevent parse
 * as fpm_http_request_path(), so the path a request is matched and routed on is
 * the path the application sees, for every form ("http://h/x", "http:/x",
 * "//h/x", "/x"); the fragment is dropped (#534, #462). "*" and a target
 * libevent did not parse are copied verbatim. */
void fpm_http_origin_form(smart_str *out, struct evhttp_request *req)
{
	const struct evhttp_uri *u = evhttp_request_get_evhttp_uri(req);
	const char *path = fpm_http_request_path(req);
	const char *query = u ? evhttp_uri_get_query(u) : NULL;

	if (!path) {
		smart_str_appends(out, evhttp_request_get_uri(req));
		return;
	}
	smart_str_appends(out, path);
	if (query) {
		smart_str_appendc(out, '?');
		smart_str_appends(out, query);
	}
}

const char *fpm_http_method_name(enum evhttp_cmd_type type)
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
int fpm_http_build_request(fpm_http_conn *c, int script_missing_hint)
{
	static const char begin_request[8] = { 0, FCGI_RESPONDER, FCGI_KEEP_CONN, 0, 0, 0, 0, 0 };
	struct evhttp_request *req = c->req;
	const struct evhttp_uri *uri = evhttp_request_get_evhttp_uri(req);
	const char *method = fpm_http_method_name(evhttp_request_get_command(req));
	const char *path = fpm_http_request_path(req);
	const char *query = uri ? evhttp_uri_get_query(uri) : NULL;
	const char *host = evhttp_request_get_host(req);
	struct evkeyval *header;
	struct evbuffer *body = evhttp_request_get_input_buffer(req);
	char *decoded, buf[64], authority[FPM_HTTP_AUTHORITY_MAX];
	int have_authority;
	int saw_host;
	smart_str filename = { 0 };
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
	TAILQ_FOREACH(header, evhttp_request_get_input_headers(req), next)
	{
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
	smart_str_appendl(&filename, decoded, path_info ? (size_t) (path_info - decoded) : decoded_len);
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
			path_info = decoded; /* whole original path, whatever split/index.php rule ran above */
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
	{
		smart_str request_uri = { 0 };

		fpm_http_origin_form(&request_uri, req);
		smart_str_0(&request_uri);
		fpm_http_param(c, "REQUEST_URI", ZSTR_VAL(request_uri.s));
		smart_str_free(&request_uri);
	}
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
	 * TLS on this connection itself (http.tls_cert, see fpm_tls_http.h -- that
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

	/* An absolute-form target's authority replaces the Host header (RFC 9112
	 * 3.2.2), so HTTP_HOST agrees with SERVER_NAME and with what the http
	 * transport sends (#534). */
	have_authority = fpm_http_absolute_authority(evhttp_request_get_uri(req), authority, sizeof(authority)) > 0;
	saw_host = 0;

	/* "Content-Type: x" -> CONTENT_TYPE, anything else -> HTTP_<UPPER_WITH_UNDERSCORES> */
	TAILQ_FOREACH(header, evhttp_request_get_input_headers(req), next)
	{
		smart_str name = { 0 };
		const char *k = header->key;
		const char *value = header->value;

		if (strcasecmp(k, "Host") == 0) {
			saw_host = 1;
			if (have_authority) {
				value = authority;
			}
		}

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
		/* A name with "_" would collide with its "-" spelling: the mapping below
		 * turns "-" into "_", so "X_Real_IP" and "X-Real-IP" both become
		 * HTTP_X_REAL_IP, and the last pair on the wire wins in $_SERVER
		 * (fcgi_hash_set replaces an existing key). A client could then
		 * override a header the reverse proxy in front set. nginx (default
		 * underscores_in_headers off) and Apache 2.4 drop such headers when
		 * they build the CGI environment; the gateway is the front server for
		 * this hop, so it does the same. Same rule in
		 * fpm_http_direct_build_env(). Issue #595. */
		if (strchr(k, '_') != NULL) {
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
			unsigned char ch = (unsigned char) *k; /* not `c`: that is this connection */

			if (ch >= 'a' && ch <= 'z') {
				smart_str_appendc(&name, (char) (ch - ('a' - 'A')));
			} else {
				smart_str_appendc(&name, ch == '-' ? '_' : (char) ch);
			}
		}
		smart_str_0(&name);
		fpm_http_param(c, ZSTR_VAL(name.s), value);
		smart_str_free(&name);
	}

	/* No Host header at all: the authority still defines the host (RFC 9112
	 * 3.2.2), same as the Host line the http transport sends. */
	if (have_authority && !saw_host) {
		fpm_http_param(c, "HTTP_HOST", authority);
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
		const char *data = (const char *) evbuffer_pullup(body, -1);
		size_t off;

		for (off = 0; off < body_len; off += FCGI_MAX_RECORD_LEN) {
			fpm_http_fcgi_record(&c->out, FCGI_STDIN, data + off, MIN(body_len - off, FCGI_MAX_RECORD_LEN));
		}
	}
	fpm_http_fcgi_record(&c->out, FCGI_STDIN, "", 0);
	return 0;
}

/* ---------------------------------------------------------------- FastCGI -> response */

void fpm_http_conn_free(fpm_http_conn *c)
{
	/* Issue #390: the connection's close callback is the connections_open
	 * gauge's, not this request's -- it stays registered on fpm_http_client_s.
	 * All this request has to do is stop being the connection's in-flight one,
	 * so the callback does not reach a c that is about to be freed. */
	if (c->client) {
		c->client->c = NULL;
		c->client = NULL;
	}
	if (c->queued) {
		TAILQ_REMOVE(&c->target->waiting, c, link);
	}
	/* http.pool_full_policy = wait (issue #309): freed here and nowhere else,
	 * so every path out of the queue -- dispatch, the reject-drain, an
	 * expired wait, and a client that disconnects while queued -- releases
	 * the timer exactly once. */
	if (c->wait_timer) {
		event_free(c->wait_timer);
		c->wait_timer = NULL;
	}
	smart_str_free(&c->params);
	smart_str_free(&c->out);
	smart_str_free(&c->cgi_headers);
	/* Issue #389: the rewritten request-target of an operator-forwarded
	 * request. NULL for every routed request. */
	free(c->upstream_uri_owned);
	free(c);
}

/* Issue #594: a CGI "Status:" value is "NNN" or "NNN reason", exactly three
 * digits, and only a final status (200..599, fpm_http_direct_status_final())
 * may become the status line. atoi() turned "abc" into 0, "99999" into itself
 * and overflowed on a longer number; a 1xx went out as the *final* answer, its
 * body dropped by libevent, and the client waited for a response that never
 * came (the shape of #451). *reason points into `value`, "" when absent.
 *
 * Issue #605: the reason goes out on the wire verbatim
 * (evhttp_send_reply_start() frames whatever it is given), so it must not
 * carry control bytes: an interior CR would split the status line, and
 * anything below 0x20 or DEL has no business in a reason phrase. A bad
 * reason is rejected (502, like a bad code) rather than stripped: stripping
 * would silently rewrite what the upstream said, while the invalid_status
 * path already logs the offending value, maps to 502 in the access log and
 * drops the rest of the untrusted reply. The scan runs over all vlen bytes
 * because `value` is strndup()ed: an embedded NUL would hide the tail from a
 * strlen()-bounded loop while truncating the strdup() below, and '\n' cannot
 * arrive at all (the caller splits lines on it), but '\r' (only the trailing
 * one is stripped), NUL and the other controls can, from any non-PHP
 * FastCGI upstream -- PHP's own header() already refuses CR, LF and NUL.
 * The WARNING below names only the first 64 bytes up to any NUL; that
 * truncation is accepted because the 502 decision does not depend on the
 * logged text. */
static bool fpm_http_parse_cgi_status(const char *value, size_t vlen, int *code, const char **reason)
{
	size_t i;

	if (vlen < 3 || !isdigit((unsigned char) value[0]) || !isdigit((unsigned char) value[1]) ||
			!isdigit((unsigned char) value[2])) {
		return false;
	}
	if (vlen == 3) {
		*reason = "";
	} else {
		if (value[3] != ' ') {
			return false;
		}
		for (i = 4; i < vlen; i++) {
			unsigned char ch = (unsigned char) value[i];

			if (ch < 0x20 || ch == 0x7f) {
				return false;
			}
		}
		*reason = value + 4;
	}
	*code = (value[0] - '0') * 100 + (value[1] - '0') * 10 + (value[2] - '0');
	return fpm_http_direct_status_final(*code);
}

/* The CGI header block is complete: "Status:" becomes the status line, the rest is copied. */
void fpm_http_start_reply(fpm_http_conn *c, size_t head_len, size_t body_off)
{
	struct evkeyvalq *out = evhttp_request_get_output_headers(c->req);
	const char *line = c->cgi_headers.s ? ZSTR_VAL(c->cgi_headers.s) : "", *end = line + head_len;
	char *reason = NULL;
	int code = HTTP_OK;
	bool invalid_status = false;

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
				const char *parsed_reason;

				free(reason);
				reason = NULL;
				if (fpm_http_parse_cgi_status(value, vlen, &code, &parsed_reason)) {
					reason = strdup(parsed_reason);
					invalid_status = false;
				} else {
					zlog(ZLOG_WARNING, "[pool %s] http: upstream sent invalid Status '%.64s', answering 502",
							c->gw->pool, value);
					invalid_status = true;
				}
			} else {
				evhttp_add_header(out, key, value);
			}
			free(key);
			free(value);
		}
		line = next;
	}

	if (invalid_status) {
		/* Nothing the upstream said can be trusted any more: drop its headers
		 * and, via discard_upstream, the rest of its reply. The 502 is a
		 * chunked reply that fpm_http_finish() ends like any other, so the
		 * request stays alive until then (an evhttp_send_error() here would
		 * complete it while the upstream is still talking). Mapping to 500
		 * instead is a maintainer decision (issue #594). */
		struct evbuffer *msg = evbuffer_new();

		evhttp_clear_headers(out);
		evhttp_add_header(out, "Content-Type", "text/plain");
		evhttp_send_reply_start(c->req, FPM_HTTP_BAD_GATEWAY, "Bad Gateway");
		evbuffer_add_printf(msg, "Bad Gateway\n");
		c->bytes_out += evbuffer_get_length(msg);
		evhttp_send_reply_chunk(c->req, msg);
		evbuffer_free(msg);
		free(reason);
		c->headers_sent = 1;
		c->discard_upstream = 1;
		c->status = FPM_HTTP_BAD_GATEWAY;
		smart_str_free(&c->cgi_headers);
		return;
	}

	/* http.pool_full_policy = wait (issue #309): observable from outside the
	 * process, added after the CGI headers were copied so a script cannot
	 * overwrite this with a header of its own. */
	if (c->queue_wait_ms >= 0) {
		char waited[32];

		snprintf(waited, sizeof(waited), "%ld", c->queue_wait_ms);
		evhttp_add_header(out, "X-Fpmng-Queue-Wait", waited);
	}
	evhttp_send_reply_start(c->req, code, reason && *reason ? reason : NULL);
	free(reason);
	c->headers_sent = 1;
	c->status = code; /* for the access log, see fpm_http_finish() */

	if (c->cgi_headers.s && body_off < ZSTR_LEN(c->cgi_headers.s)) {
		fpm_http_response_chunk(c, ZSTR_VAL(c->cgi_headers.s) + body_off, ZSTR_LEN(c->cgi_headers.s) - body_off);
	}
	smart_str_free(&c->cgi_headers);
}

void fpm_http_stdout(fpm_http_conn *c, const char *data, size_t len)
{
	size_t scan_from, i;
	const char *h;

	if (c->discard_upstream) {
		return; /* issue #594: the 502 is already on the wire */
	}
	if (c->headers_sent) {
		fpm_http_response_chunk(c, data, len);
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

/* Issue #533: the upstream failed after the status line and headers went to
 * the client. evhttp_send_reply_end() would put the terminating 0-chunk on the
 * wire, and the client (or a cache between us and it) would store a truncated
 * body as a complete one. The only truthful thing left to send is a message
 * that does not end: access-log the request, then shutdown() the client socket
 * -- the same shape as fpm_direct_stream_abort() in fpm_http_direct.c -- so
 * libevent finds the connection dead on its next pass, frees the request there
 * and runs fpm_http_client_closed() for the gauge and the index. The close
 * callback stays registered on purpose; fpm_http_conn_free() has detached this
 * request from it. A reply framed by Content-Length is cut short of its
 * length, a chunked one of its terminator; an HTTP/1.0 client gets a
 * close-delimited reply, which no close can mark as incomplete. */
void fpm_http_finish_truncated(fpm_http_conn *c)
{
	struct evhttp_connection *evcon = evhttp_request_get_connection(c->req);
	struct bufferevent *bev = evcon ? evhttp_connection_get_bufferevent(evcon) : NULL;
	evutil_socket_t fd = bev ? bufferevent_getfd(bev) : -1;

	fpm_http_log_response(c->gw, c->req, c->remote_addr[0] ? c->remote_addr : c->peer_addr,
			c->remote_user, c->status, c->bytes_out,
			c->log_target ? c->log_target : c->target->pool);
	fpm_http_conn_free(c);
	if (fd >= 0) {
		shutdown(fd, SHUT_RDWR);
	}
}

/* The pool is done with the request (END_REQUEST seen or the connection failed).
 * `explained` says the reason is already in the log -- a clean EOF needs no
 * line at all, and fpm_http_upstream_fail() writes its own for the case it can
 * name (issue #118) -- so only the unexplained loss is reported from here. */
void fpm_http_finish(fpm_http_conn *c, int explained)
{
	/* Issue #596: the last piece of a read can pause the upstream and the
	 * END_REQUEST behind it finish the request in the same read; nothing would
	 * re-arm the read event of an upstream that has no idle timeout. */
	fpm_http_response_resume(c);
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
			c->remote_user, c->status, c->bytes_out,
			c->log_target ? c->log_target : c->target->pool); /* #389: "operator" for an operator-forwarded request */
	fpm_http_conn_free(c);
}

#endif /* HAVE_FPM_HTTP */
