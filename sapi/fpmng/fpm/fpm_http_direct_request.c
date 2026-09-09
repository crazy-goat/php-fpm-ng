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

#include <ctype.h>
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
	if (strncmp(script, root, strlen(root)) || (strcmp(root, "/") && script[strlen(root)] != '/')) {
		return -1;
	}
	return 0;
}

int fpm_http_direct_validate_common(struct fpm_worker_pool_s *wp, const struct fpm_http_direct_labels *labels)
{
	struct fpm_worker_pool_config_s *c = wp->config;
	const char *p = c->set_directives;
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
	/* Gateway options must not silently appear to protect a direct worker.
	 * Use an allow-list here so future http.* directives are rejected too. */
	while (p && (p = strstr(p, ";http."))) {
		const char *end = strchr(++p, ';');
		size_t len = end ? (size_t) (end - p) : strlen(p);
		if (!((len == sizeof("http.front_controller") - 1 && !strncmp(p, "http.front_controller", len)) ||
			(len == sizeof("http.read_timeout") - 1 && !strncmp(p, "http.read_timeout", len)) ||
			(len == sizeof("http.max_body") - 1 && !strncmp(p, "http.max_body", len)))) {
			zlog(ZLOG_ALERT, "[pool %s] '%.*s' is not supported by %s",
				c->name, (int) len, p, labels->type_label);
			return -1;
		}
	}
	if (c->http_read_timeout <= 0 || c->http_max_body == 0 || c->http_max_body > 32 * 1024 * 1024) {
		zlog(ZLOG_ALERT, "[pool %s] %s requires http.read_timeout > 0 and http.max_body between 1 and 32M",
			c->name, labels->subject);
		return -1;
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
 * assume FPM_HTTP_DIRECT_HEADER_NAME_MAX. */
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
		if (strlen(kv->key) > FPM_HTTP_DIRECT_HEADER_NAME_MAX) {
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
	ENV("CONTENT_LENGTH", length);
	ENV("CONTENT_TYPE", evhttp_find_header(headers, "Content-Type"));
	for (kv = headers->tqh_first; kv; kv = kv->next.tqe_next) {
		char name[FPM_HTTP_DIRECT_HEADER_NAME_MAX + sizeof("HTTP_")];
		size_t i, len = strlen(kv->key);

		/* Content-* are already above under their CGI names; "Proxy" has no
		 * CGI meaning at all and HTTP_PROXY is read as an outbound proxy by
		 * several client libraries (httpoxy). */
		if (!strcasecmp(kv->key, "Content-Type") || !strcasecmp(kv->key, "Content-Length") ||
			!strcasecmp(kv->key, "Proxy")) {
			continue;
		}
		if (len > FPM_HTTP_DIRECT_HEADER_NAME_MAX) {
			return -1;
		}
		memcpy(name, "HTTP_", 5);
		for (i = 0; i < len; i++) {
			unsigned char c = (unsigned char) kv->key[i];
			name[5 + i] = c == '-' ? '_' : (char) toupper(c);
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
