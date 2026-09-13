/* fpm-ng: the request side of the HTTP-direct transport, shared by
 * pool.type = http-direct (fpm_http_direct.c) and pool.executor = worker
 * (fpm_http_direct_worker.c), issue #74.
 *
 * The two executors differ in who owns the event loop, so their response and
 * lifecycle paths differ for a real reason and stay where they are. What a
 * request *is* does not differ: the same directives are refused, the same
 * script is resolved the same way, the same CGI variables are derived from the
 * same evhttp request, and the same headers and statuses must never reach the
 * wire because this transport owns framing. Those lived in two copies, written
 * by reading one another, and had already drifted twice — the accepted status
 * range and the dropped-header contract, both fixed in one copy each. One
 * implementation removes the drift by construction; a caller that wants the
 * other behaviour now has to change it for both, in the open.
 */
#include "fpm_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/keyvalq_struct.h>

#include "php.h"
#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_http_direct_request.h"
#include "fpm_http_direct_tls.h"
#include "fpm_http_acl.h"
#include "zlog.h"

/* Resolves the front controller (the worker script, under the worker
 * executor) against `base`, or against the current directory when base is
 * NULL — the master validates the configured chdir, the child validates what
 * it actually chdir'd into. Both must end up with a regular file inside the
 * root, so a bad deployment path is caught before FPM starts respawning
 * children that cannot open their script. */
int fpm_http_direct_resolve_script(const char *base, const char *front_controller,
	char root[PATH_MAX], char script[PATH_MAX])
{
	char candidate[PATH_MAX];
	struct stat st;

	if (base ? !realpath(base, root) : !getcwd(root, PATH_MAX)) {
		return -1;
	}
	if (snprintf(candidate, PATH_MAX, "%s%s", root, front_controller) >= PATH_MAX ||
		!realpath(candidate, script) || stat(script, &st) < 0 || !S_ISREG(st.st_mode)) {
		return -1;
	}
	/* The '/' test is what keeps /var/wwwroot out of a /var/www root; the
	 * strcmp guards the one root for which it would reject everything. */
	if (strncmp(script, root, strlen(root)) != 0 || (strcmp(root, "/") != 0 && script[strlen(root)] != '/')) {
		return -1;
	}
	return 0;
}

/* labels->extra_directives, compared the same way as the shared entries: the
 * directive list is a ';'-separated run of names, not NUL-terminated ones. */
static bool fpm_http_direct_directive_extra(const struct fpm_http_direct_labels *labels,
	const char *p, size_t len)
{
	const char *const *extra;

	for (extra = labels->extra_directives; extra && *extra; extra++) {
		if (strlen(*extra) == len && !strncmp(p, *extra, len)) {
			return true;
		}
	}
	return false;
}

/* Whether the pool file itself set this directive, as opposed to it holding
 * whatever fpm_conf.c's global default left there. set_directives is a
 * ';'-separated run of names, ';' both before and after each one. */
static bool fpm_http_direct_declared(const char *set_directives, const char *name)
{
	const char *p = set_directives;
	size_t len = strlen(name);

	while (p && (p = strstr(p, name))) {
		if (p > set_directives && p[-1] == ';' && (p[len] == ';' || p[len] == '\0')) {
			return true;
		}
		p += len;
	}

	return false;
}

/* INI value set in the pool, or NULL when only php.ini applies. Same shape and
 * same admin-before-value order as fpm_worker_pool_ini() in
 * fpm_http_direct_worker.c; that one is static there and this file is the
 * shared half, so the lookup is repeated rather than exported. */
static const char *fpm_http_direct_pool_ini(struct fpm_worker_pool_s *wp, const char *key)
{
	struct key_value_s *kv;

	for (kv = wp->config->php_admin_values; kv; kv = kv->next) {
		if (!strcasecmp(kv->key, key)) {
			return kv->value;
		}
	}
	for (kv = wp->config->php_values; kv; kv = kv->next) {
		if (!strcasecmp(kv->key, key)) {
			return kv->value;
		}
	}
	return NULL;
}

int fpm_http_direct_validate_common(struct fpm_worker_pool_s *wp, const struct fpm_http_direct_labels *labels)
{
	struct fpm_worker_pool_config_s *c = wp->config;
	const char *p = c->set_directives;
	const char *user_ini;
	char root[PATH_MAX], script[PATH_MAX];

	if (c->pm != PM_STYLE_STATIC) {
		zlog(ZLOG_ALERT, "[pool %s] %s requires pm = static", c->name, labels->subject);
		return -1;
	}
	if (!c->chdir || c->chdir[0] != '/' || !c->http_front_controller || c->http_front_controller[0] != '/' ||
		strstr(c->http_front_controller, "..") || strchr(c->http_front_controller, '\\')) {
		zlog(ZLOG_ALERT, "[pool %s] %s requires an absolute chdir and a root-relative http.front_controller%s "
			"without '..' or backslashes", c->name, labels->subject, labels->chdir_note);
		return -1;
	}
	if (c->listen_allowed_clients && *c->listen_allowed_clients) {
		struct fpm_http_acl_s *tmp = NULL;

		/* Parsed here, in the master, and thrown away: the child parses it
		 * again for real, but a child that finds it malformed can only exit,
		 * and the master would fork a replacement immediately -- the pool
		 * would spin instead of failing. A bad address must stop `-t`. */
		if (fpm_http_acl_parse(c->name, "listen.allowed_clients", c->listen_allowed_clients, &tmp) != 0) {
			return -1; /* fpm_http_acl_parse() already logged which address is bad */
		}
		fpm_http_acl_free(tmp);
	}
	/* .user.ini for a direct pool is read from the front controller's own
	 * directory upwards to the document root (fpm_http_direct_user_ini.c), so
	 * the file name is walked once per directory. A name carrying a path
	 * separator would make each of those probes name a directory other than
	 * the one being walked -- "../../etc/evil.ini" from three levels down
	 * leaves the root entirely -- and the containment the rest of this
	 * transport is built on would be decided by an ini string. Refused here,
	 * in the master, so `-t` says so rather than every child exiting in turn.
	 * The child refuses the same value again (init_child), because php.ini
	 * could be replaced between the test and the fork. */
	user_ini = fpm_http_direct_pool_ini(wp, "user_ini.filename");
	if (!user_ini) {
		user_ini = zend_ini_string("user_ini.filename", sizeof("user_ini.filename") - 1, 0);
	}
	if (user_ini && strchr(user_ini, '/')) {
		zlog(ZLOG_ALERT, "[pool %s] %s: user_ini.filename must be a bare file name, not '%s': "
			"a separator in it would let the per-directory ini scan leave the document root",
			c->name, labels->subject, user_ini);
		return -1;
	}
	/* Gateway options must not silently appear to protect a direct worker.
	 * Use an allow-list here so future http.* directives are rejected too. */
#define FPM_HTTP_DIRECT_DIRECTIVE(p, len, name) \
	((len) == sizeof(name) - 1 && !strncmp((p), (name), (len)))
	while (p && (p = strstr(p, ";http."))) {
		const char *end = strchr(++p, ';');
		size_t len = end ? (size_t) (end - p) : strlen(p);
		if (!(FPM_HTTP_DIRECT_DIRECTIVE(p, len, "http.front_controller") ||
			FPM_HTTP_DIRECT_DIRECTIVE(p, len, "http.read_timeout") ||
			FPM_HTTP_DIRECT_DIRECTIVE(p, len, "http.max_body") ||
			/* issue #61 */
			FPM_HTTP_DIRECT_DIRECTIVE(p, len, "http.max_connections") ||
			FPM_HTTP_DIRECT_DIRECTIVE(p, len, "http.max_connections_per_client") ||
			/* issue #55. http.plain_listen is deliberately NOT here: a direct
			 * pool accepts on one socket, so there is nowhere to put a second
			 * listener, and accepting the directive would read as if there
			 * were. */
			FPM_HTTP_DIRECT_DIRECTIVE(p, len, "http.tls_cert") ||
			FPM_HTTP_DIRECT_DIRECTIVE(p, len, "http.tls_key") ||
			FPM_HTTP_DIRECT_DIRECTIVE(p, len, "http.tls_min_version") ||
			FPM_HTTP_DIRECT_DIRECTIVE(p, len, "http.tls_sni_cert") ||
			FPM_HTTP_DIRECT_DIRECTIVE(p, len, "http.tls_reload_check") ||
			fpm_http_direct_directive_extra(labels, p, len))) {
			zlog(ZLOG_ALERT, "[pool %s] '%.*s' is not supported by %s",
				c->name, (int) len, p, labels->type_label);
			return -1;
		}
	}
#undef FPM_HTTP_DIRECT_DIRECTIVE
	/* http.static defaults to on for the http gateway, where a document root
	 * and a web server are the whole point. A direct pool's root is its chdir,
	 * which is chosen to hold the application -- vendor/, .env-adjacent config,
	 * whatever the framework keeps next to its front controller -- so inheriting
	 * that default would start serving those files to the internet on upgrade,
	 * from pools whose owner never asked for a file server. Hence: off unless
	 * the pool says otherwise, and the asymmetry is documented in
	 * docs/http-direct.md rather than left to be discovered.
	 *
	 * Here rather than in the executor that implements it, because the default
	 * has to be off for the executor that does not: the loop above rejects
	 * http.static unless the executor listed it, and a pool that cannot say
	 * "yes" must not have "yes" assumed for it. */
	if (!fpm_http_direct_declared(c->set_directives, "http.static")) {
		c->http_static = 0;
	}
	if (c->http_read_timeout <= 0 || c->http_max_body == 0 || c->http_max_body > 32 * 1024 * 1024) {
		zlog(ZLOG_ALERT, "[pool %s] %s requires http.read_timeout > 0 and http.max_body between 1 and 32M",
			c->name, labels->subject);
		return -1;
	}
	/* issue #61. The upper bound is not a resource limit -- it is there so a
	 * typo like 100000000 is a configuration error rather than a worker that
	 * tracks a list it can never fill. Both are per worker, and a per-client
	 * limit above the total one can never bite, which is a mistake worth
	 * naming rather than silently accepting. */
	if (c->http_max_connections < 0 || c->http_max_connections > 1000000 ||
		c->http_max_connections_per_client < 0 ||
		c->http_max_connections_per_client > 1000000) {
		zlog(ZLOG_ALERT, "[pool %s] %s requires http.max_connections and "
			"http.max_connections_per_client between 0 (unlimited) and 1000000",
			c->name, labels->subject);
		return -1;
	}
	/* The per-client cap is enforced by walking this worker's connection list
	 * once per accepted connection, so the list has to be bounded by something
	 * -- and the only thing that bounds it is the total cap. Without it a
	 * flood from many distinct addresses would make the accept path O(n) in
	 * the connections already held, which is the amplifier the directive is
	 * there to prevent. Required rather than implied: a default this file
	 * invented would be a number nobody measured. */
	if (c->http_max_connections_per_client > 0 && c->http_max_connections <= 0) {
		zlog(ZLOG_ALERT, "[pool %s] %s: http.max_connections_per_client requires "
			"http.max_connections, which is what bounds the list it is counted against",
			c->name, labels->subject);
		return -1;
	}
	if (c->http_max_connections > 0 && c->http_max_connections_per_client > c->http_max_connections) {
		zlog(ZLOG_ALERT, "[pool %s] %s: http.max_connections_per_client (%d) is above "
			"http.max_connections (%d), so it can never apply",
			c->name, labels->subject, c->http_max_connections_per_client, c->http_max_connections);
		return -1;
	}
	if (fpm_http_direct_tls_validate(wp) < 0) {
		return -1; /* logged there, with the pool name and what is wrong */
	}
	/* The master is not chrooted, so under `chroot` the pool's chdir names a
	 * directory that only exists once the child has called chroot(2). Prepend
	 * it exactly the way fpm_conf.c does when it checks that the chdir exists
	 * (fpm_conf.c, "the chdir path '%s' within the chroot path '%s'"), so the
	 * startup check looks at the same directory the child will. The child's
	 * own resolution passes base = NULL and runs after chroot(), where the
	 * unprefixed path is the right one. */
	if (c->chroot && *c->chroot) {
		char base[PATH_MAX];

		if ((size_t) snprintf(base, sizeof(base), "%s%s", c->chroot, c->chdir) >= sizeof(base)) {
			zlog(ZLOG_ALERT, "[pool %s] %s: chroot + chdir is longer than PATH_MAX",
				c->name, labels->script_context);
			return -1;
		}
		if (fpm_http_direct_resolve_script(base, c->http_front_controller, root, script) < 0) {
			zlog(ZLOG_ALERT, "[pool %s] %s: %s must be a regular file inside chdir, "
				"which is resolved inside chroot '%s' here",
				c->name, labels->script_context, labels->script_noun, c->chroot);
			return -1;
		}
		return 0;
	}
	if (fpm_http_direct_resolve_script(c->chdir, c->http_front_controller, root, script) < 0) {
		zlog(ZLOG_ALERT, "[pool %s] %s: %s must be a regular file inside chdir",
			c->name, labels->script_context, labels->script_noun);
		return -1;
	}
	return 0;
}

/* NULL for anything evhttp_set_allowed_methods() is not configured to accept,
 * which both executors turn into 400 rather than guessing a method. */
const char *fpm_http_direct_method(enum evhttp_cmd_type command)
{
	switch (command) {
		case EVHTTP_REQ_GET: return "GET";
		case EVHTTP_REQ_POST: return "POST";
		case EVHTTP_REQ_HEAD: return "HEAD";
		case EVHTTP_REQ_PUT: return "PUT";
		case EVHTTP_REQ_DELETE: return "DELETE";
		case EVHTTP_REQ_OPTIONS: return "OPTIONS";
		case EVHTTP_REQ_PATCH: return "PATCH";
		default: return NULL;
	}
}

/* Origin-form only: the script is selected solely by configuration, never by
 * the URI, the Host, PATH_INFO, or any client-supplied CGI-looking header.
 * Header names are bounded here, once, so that everything downstream may
 * assume FPM_HTTP_HEADER_NAME_MAX. */
bool fpm_http_direct_request_acceptable(struct evhttp_request *http)
{
	const char *uri = evhttp_request_get_uri(http);
	const struct evhttp_uri *parsed = evhttp_request_get_evhttp_uri(http);
	struct evkeyval *kv;

	if (!uri || uri[0] != '/' || !parsed || evhttp_uri_get_fragment(parsed) ||
		!fpm_http_direct_method(evhttp_request_get_command(http))) {
		return false;
	}
	for (kv = evhttp_request_get_input_headers(http)->tqh_first; kv; kv = kv->next.tqe_next) {
		if (strlen(kv->key) > FPM_HTTP_HEADER_NAME_MAX) {
			return false;
		}
	}
	return true;
}

/* Only the libevent getters below can return NULL, but a macro cannot test
 * that without gcc -Waddress rejecting the same test on the local buffers and
 * literals ("the address of 'protocol' will always evaluate as true"). A
 * function parameter has decayed to a pointer by then. */
static const char *fpm_http_direct_or_empty(const char *value)
{
	return value ? value : "";
}

/* Assumes fpm_http_direct_request_acceptable(). The callback decides what an
 * environment entry is: an evkeyvalq for the classic transport's SAPI getenv,
 * a PHP array for the worker's fpmng_worker_request_env(). */
int fpm_http_direct_build_env(struct evhttp_request *http, const struct fpm_http_direct_env_source *source,
	fpm_http_direct_env_cb emit, void *ctx)
{
	const struct evhttp_uri *parsed = evhttp_request_get_evhttp_uri(http);
	struct evkeyvalq *headers = evhttp_request_get_input_headers(http);
	struct evkeyval *kv;
	char length[32], remote_port[16], protocol[32];
	char *peer = NULL;
	ev_uint16_t port = 0;

	snprintf(length, sizeof(length), "%zu", evbuffer_get_length(evhttp_request_get_input_buffer(http)));
	evhttp_connection_get_peer(evhttp_request_get_connection(http), &peer, &port);
	snprintf(remote_port, sizeof(remote_port), "%u", (unsigned) port);
	snprintf(protocol, sizeof(protocol), "HTTP/%d.%d", http->major, http->minor);
#define ENV(key, value) do { if (emit(ctx, key, fpm_http_direct_or_empty(value))) return -1; } while (0)
	ENV("REQUEST_METHOD", fpm_http_direct_method(evhttp_request_get_command(http)));
	ENV("REQUEST_URI", evhttp_request_get_uri(http));
	ENV("QUERY_STRING", evhttp_uri_get_query(parsed));
	ENV("SCRIPT_FILENAME", source->script);
	ENV("SCRIPT_NAME", source->front_controller);
	ENV("PHP_SELF", source->front_controller);
	ENV("PATH_INFO", evhttp_uri_get_path(parsed));
	ENV("DOCUMENT_ROOT", source->root);
	ENV("SERVER_PROTOCOL", protocol);
	ENV("SERVER_SOFTWARE", source->server_software);
	ENV("GATEWAY_INTERFACE", "CGI/1.1");
	ENV("SERVER_ADDR", source->server_addr);
	ENV("SERVER_PORT", source->server_port);
	ENV("SERVER_NAME", evhttp_request_get_host(http));
	ENV("REMOTE_ADDR", peer);
	ENV("REMOTE_PORT", remote_port);
	/* Not "off" when there is no TLS: CGI has no negative form for HTTPS and
	 * PHP treats any non-empty value as on, so the variable is simply absent
	 * on a plain pool -- which is what $_SERVER['HTTPS'] tests expect and what
	 * the gateway already does (fpm_http.c). REQUEST_SCHEME is always set:
	 * unlike HTTPS it has a meaningful "http". */
	ENV("REQUEST_SCHEME", source->tls ? "https" : "http");
	if (source->tls) {
		ENV("HTTPS", "on");
	}
	ENV("CONTENT_LENGTH", length);
	ENV("CONTENT_TYPE", evhttp_find_header(headers, "Content-Type"));
	for (kv = headers->tqh_first; kv; kv = kv->next.tqe_next) {
		char name[FPM_HTTP_HEADER_NAME_MAX + sizeof("HTTP_")];
		size_t i, len = strlen(kv->key);

		/* Content-* are already above under their CGI names; "Proxy" has no
		 * CGI meaning at all and HTTP_PROXY is read as an outbound proxy by
		 * several client libraries (httpoxy). */
		if (!strcasecmp(kv->key, "Content-Type") || !strcasecmp(kv->key, "Content-Length") ||
			!strcasecmp(kv->key, "Proxy")) {
			continue;
		}
		if (len > FPM_HTTP_HEADER_NAME_MAX) {
			return -1;
		}
		/* Explicit range, not toupper(): LC_CTYPE belongs to the application in
		 * this child, and the locale changes this mapping for plain US-ASCII
		 * input. In tr_TR.UTF-8 and az_AZ.UTF-8 toupper('i') returns 'i' -- the
		 * Turkish capital of 'i' is U+0130, which does not fit the single-byte
		 * table -- so "If-Modified-Since" became HTTP_IF_MODiFiED_SiNCE and
		 * nothing reading $_SERVER['HTTP_IF_MODIFIED_SINCE'] found it. Served and
		 * measured on 192.168.8.50, glibc 2.43, php-8.5.9, 2026-09-09, with the
		 * pre-fix binary. de_DE.ISO-8859-1 remaps 30 bytes above 0x7F on top of
		 * that.
		 *
		 * Reachable through pool.executor = worker, where the boot script calls
		 * setlocale() once and every later request's environment is derived
		 * inside that same PHP request. Not reproducible on the classic executor
		 * with today's php-src: ext/standard's request shutdown puts LC_ALL back
		 * to "C" when setlocale() was called (ext/standard/basic_functions.c:448
		 * in php-8.5.9), and request N+1's environment is built before its script
		 * runs. That is upstream's bookkeeping, not a property of this transport,
		 * so it is not what the mapping relies on: the CGI key a header lands
		 * under is a security boundary -- the Proxy and Content-* exclusions
		 * above are enforced by name -- and must not depend on process state the
		 * application chose. Issue #105; same class as #102 on the response side. */
		memcpy(name, "HTTP_", 5);
		for (i = 0; i < len; i++) {
			unsigned char c = (unsigned char) kv->key[i];

			if (c >= 'a' && c <= 'z') {
				name[5 + i] = (char) (c - ('a' - 'A'));
			} else {
				name[5 + i] = c == '-' ? '_' : (char) c;
			}
		}
		name[5 + len] = '\0';
		ENV(name, kv->value);
	}
#undef ENV
	return 0;
}

/* This transport owns framing. An application-supplied length, connection or
 * Transfer-Encoding header must not desynchronize the next keep-alive
 * request, so it never reaches the response. */
bool fpm_http_direct_header_dropped(const char *name)
{
	return !strcasecmp(name, "Content-Length") || !strcasecmp(name, "Transfer-Encoding") ||
		!strcasecmp(name, "Connection") || !strcasecmp(name, "Keep-Alive") ||
		!strcasecmp(name, "Upgrade") || !strcasecmp(name, "Trailer");
}

/* Final statuses only. evhttp_send_reply() would happily emit a 1xx status
 * line as if it were the response and still frame and append the body,
 * leaving the next keep-alive response behind bytes the client never read as
 * one. */
bool fpm_http_direct_status_final(long status)
{
	return status >= 200 && status <= 599;
}

/* libevent omits the framing headers for 204/205/304 but still appends a
 * supplied body, and a body on a HEAD response is unframed by definition:
 * either way those bytes would appear in front of the next keep-alive
 * response. The body is dropped, not the status. */
bool fpm_http_direct_status_bodyless(struct evhttp_request *http, int status)
{
	return evhttp_request_get_command(http) == EVHTTP_REQ_HEAD ||
		status == 204 || status == 205 || status == 304;
}

/* A response header name is an RFC 9110 token. libevent 2.1 already rejects
 * CR and LF in both key and value — measured: evhttp_add_header() returns -1
 * for "X-A\r\nInjected" and for a value containing CRLF — so there is no
 * response-splitting vector here, but it stores a key containing a space or a
 * colon verbatim and emits a malformed header line. Reject those ourselves.
 *
 * Both executors need it, and neither gets it for free. `header()` rejects
 * only CR, LF and NUL in the whole line (main/SAPI.c:758-773 in php-8.5), so
 * "X Y: v", "X\tY: v" and ": v" all reach sapi_module.send_headers and, before
 * issue #102, the wire; the worker executor takes its names from a userland
 * array, which additionally admits an embedded colon. Measured on
 * 192.168.8.50 with php-8.5.9, 2026-09-09. */
bool fpm_http_direct_header_name_ok(const char *name)
{
	const char *c;

	if (!*name) {
		return false;
	}
	for (c = name; *c; c++) {
		/* Explicit ranges, not isalnum(): LC_CTYPE belongs to the application
		 * here, and setlocale(LC_ALL, 'de_DE.ISO-8859-1') makes isalnum(0xE9)
		 * true, so header("X-Caf\xE9: v") would pass a check whose whole
		 * purpose is to enforce a US-ASCII token. */
		if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) {
			continue;
		}
		if (!strchr("!#$%&'*+-.^_`|~", *c)) {
			return false;
		}
	}
	return true;
}

/* One response-header budget, spent on the same bytes by whoever serves the
 * response. The two executors used to keep their own arithmetic and disagreed
 * twice over (issue #104): the classic transport charged the whole raw
 * header() line before it knew whether the header would be dropped, so bytes
 * that never reached the wire counted; the worker executor charged
 * name + value and neither the colon, the space, nor the CRLF. A response near
 * the cap could therefore be served by one executor and answered 500 by the
 * other.
 *
 * What is counted is what the transport writes: `name: value\r\n`, and only
 * for a header that is actually emitted. A dropped framing header
 * (fpm_http_direct_header_dropped()) and the `Status:` pseudo-header are free
 * because neither appears on the wire. Called once per emitted header, so it
 * charges rather than re-totals, and the caller keeps the running total.
 *
 * Refusing without charging keeps the total below the cap on the way out; the
 * caller answers 500 on false, since a response with a header silently
 * dropped is worse than an error status. */
bool fpm_http_direct_header_charge(size_t *total, const char *name, size_t value_len)
{
	/* ": " + CRLF. Overflow-safe by subtraction: value_len comes from a
	 * zend_string an application controls and name + value + 4 could wrap on
	 * a 32-bit size_t. */
	size_t line = strlen(name) + 4;

	if (line > FPM_HTTP_HEADERS_MAX ||
		value_len > FPM_HTTP_HEADERS_MAX - line ||
		line + value_len > FPM_HTTP_HEADERS_MAX - *total) {
		return false;
	}
	*total += line + value_len;
	return true;
}

/* The rejected name is by construction not a token and header() filters only
 * CR, LF and NUL, so an application that builds a header name out of request
 * input (header($_GET['h'] . ': v') — the case that reaches the check at all)
 * can put terminal escapes into whatever reads the error log. Escaped, not
 * dropped: the operator still has to be able to recognise the name. SP is
 * escaped along with the control bytes on purpose — a leading or trailing
 * space is one of the reachable causes and is invisible inside the quotes. */
const char *fpm_http_direct_header_name_escape(const char *name, char *out, size_t size)
{
	size_t o = 0;
	const char *c;

	for (c = name; *c && o + 5 < size; c++) {
		if (*c >= 0x21 && *c <= 0x7e && *c != '\\') {
			out[o++] = *c;
		} else {
			o += (size_t) snprintf(out + o, size - o, "\\x%02x", (unsigned char) *c);
		}
	}
	out[o] = '\0';
	return out;
}
