/* Experimental HTTP transport inside a classic FPM worker. The master still
 * owns the children; only their request loop and SAPI I/O change. Unlike the
 * gateway, this event loop cannot progress while PHP is executing. */
#include "fpm_config.h"

#include <ctype.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/keyvalq_struct.h>
#include <event2/buffer.h>

#include "php.h"
#include "php_main.h"
#include "php_variables.h"
#include "fopen_wrappers.h"
#include "SAPI.h"
#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_http_direct.h"
#include "fpm_php.h"
#include "fpm_request.h"
#include "fpm_stdio.h"
#include "zlog.h"

#define FPM_DIRECT_HEADERS_MAX (64 * 1024)
#define FPM_DIRECT_RESPONSE_MAX (8 * 1024 * 1024)
#define FPM_DIRECT_PENDING_MAX 16

const char *const fpm_http_direct_rejects[] = {
	"fiber.", "supervisor.", "cron.", "chroot", "listen.allowed_clients",
	"pm.status_path", "pm.status_listen", "ping.path", "ping.response",
	"access.log", "access.format", "access.suppress_path", NULL
};

struct fpm_direct_worker {
	struct fpm_worker_pool_s *wp;
	struct event_base *base;
	struct evhttp *http;
	struct evhttp_bound_socket *listener;
	char root[PATH_MAX];
	char script[PATH_MAX];
	char server_addr[NI_MAXHOST];
	char server_port[NI_MAXSERV];
	unsigned requests;
	unsigned pending;
};

struct fpm_direct_request {
	struct evhttp_request *http;
	struct evkeyvalq env;
	struct evbuffer *output;
	int status;
	bool overflow;
};

static struct fpm_direct_request *fpm_direct_current;
static volatile sig_atomic_t fpm_direct_stopping;

int fpm_http_direct_validate(struct fpm_worker_pool_s *wp)
{
	struct fpm_worker_pool_config_s *c = wp->config;
	const char *p = c->set_directives;
	char root[PATH_MAX], script[PATH_MAX], candidate[PATH_MAX];
	struct stat st;

	if (c->pm != PM_STYLE_STATIC) {
		zlog(ZLOG_ALERT, "[pool %s] http-direct requires pm = static", c->name);
		return -1;
	}
	if (!c->chdir || c->chdir[0] != '/' || !c->http_front_controller || c->http_front_controller[0] != '/' ||
		strstr(c->http_front_controller, "..") || strchr(c->http_front_controller, '\\')) {
		zlog(ZLOG_ALERT, "[pool %s] http-direct requires an absolute chdir and a root-relative http.front_controller without '..' or backslashes", c->name);
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
			zlog(ZLOG_ALERT, "[pool %s] '%.*s' is not supported by pool.type = http-direct", c->name, (int) len, p);
			return -1;
		}
	}
	if (c->http_read_timeout <= 0 || c->http_max_body == 0 || c->http_max_body > 32 * 1024 * 1024) {
		zlog(ZLOG_ALERT, "[pool %s] http-direct requires http.read_timeout > 0 and http.max_body between 1 and 32M", c->name);
		return -1;
	}
	/* Catch a bad deployment path before FPM starts repeatedly respawning
	 * children that cannot initialize their fixed front controller. */
	if (!realpath(c->chdir, root) ||
		snprintf(candidate, sizeof(candidate), "%s%s", root, c->http_front_controller) >= (int) sizeof(candidate) ||
		!realpath(candidate, script) || stat(script, &st) < 0 || !S_ISREG(st.st_mode) ||
		strncmp(script, root, strlen(root)) || (strcmp(root, "/") && script[strlen(root)] != '/')) {
		zlog(ZLOG_ALERT, "[pool %s] http-direct: front controller must be a regular file inside chdir", c->name);
		return -1;
	}
	return 0;
}

static void fpm_direct_stop(int signo)
{
	(void) signo;
	fpm_direct_stopping = 1;
}

static void fpm_direct_tick(evutil_socket_t fd, short events, void *arg)
{
	struct fpm_direct_worker *w = arg;
	(void) fd;
	(void) events;
	if (fpm_direct_stopping) {
		if (w->listener) {
			evhttp_del_accept_socket(w->http, w->listener);
			w->listener = NULL;
		}
		if (!w->pending) {
			event_base_loopbreak(w->base);
		}
	}
}

static char *fpm_direct_getenv(const char *name, size_t len)
{
	(void) len;
	const char *value = fpm_direct_current ? evhttp_find_header(&fpm_direct_current->env, name) : NULL;
	return (char *) (value ? value : getenv(name));
}

static void fpm_direct_register_variables(zval *array)
{
	struct evkeyval *kv;
	php_import_environment_variables(array);
	for (kv = fpm_direct_current->env.tqh_first; kv; kv = kv->next.tqe_next) {
		char *value = estrdup(kv->value);
		size_t len = strlen(value);
		if (sapi_module.input_filter(PARSE_SERVER, kv->key, &value, len, &len)) {
			php_register_variable_safe(kv->key, value, len, array);
		}
		efree(value);
	}
}

static char *fpm_direct_cookies(void)
{
	return (char *) evhttp_find_header(evhttp_request_get_input_headers(fpm_direct_current->http), "Cookie");
}

static size_t fpm_direct_read_post(char *buffer, size_t size)
{
	int n = evbuffer_remove(evhttp_request_get_input_buffer(fpm_direct_current->http), buffer, size);
	return n < 0 ? 0 : (size_t) n;
}

static size_t fpm_direct_write(const char *str, size_t len)
{
	struct fpm_direct_request *r = fpm_direct_current;
	if (!r) {
		return 0;
	}
	if (!r->overflow && (len > FPM_DIRECT_RESPONSE_MAX - evbuffer_get_length(r->output) ||
		evbuffer_add(r->output, str, len) < 0)) {
		r->overflow = true;
	}
	/* Do not bail out from a shutdown callback. The bounded buffer is discarded
	 * and converted to 500 after the complete PHP shutdown sequence. */
	return len;
}

static int fpm_direct_send_headers(sapi_headers_struct *headers)
{
	struct fpm_direct_request *r = fpm_direct_current;
	struct evkeyvalq *out = evhttp_request_get_output_headers(r->http);
	sapi_header_struct *h;
	zend_llist_position pos;
	size_t total = 0;

	r->status = headers->http_response_code;
	for (h = zend_llist_get_first_ex(&headers->headers, &pos); h;
		h = zend_llist_get_next_ex(&headers->headers, &pos)) {
		char *colon = memchr(h->header, ':', h->header_len);
		char *name, *value;
		if (!colon) {
			continue;
		}
		total += h->header_len;
		if (total > FPM_DIRECT_HEADERS_MAX) {
			r->overflow = true;
			break;
		}
		name = estrndup(h->header, colon - h->header);
		value = colon + 1;
		while (*value == ' ' || *value == '\t') {
			value++;
		}
		/* This transport owns framing. An application-supplied length or
		 * Transfer-Encoding must not desynchronize the next keep-alive request. */
		if (!strcasecmp(name, "Status")) {
			r->status = atoi(value);
		} else if (strcasecmp(name, "Content-Length") && strcasecmp(name, "Transfer-Encoding") &&
			strcasecmp(name, "Connection") && strcasecmp(name, "Keep-Alive") &&
			strcasecmp(name, "Upgrade") && strcasecmp(name, "Trailer")) {
			if (evhttp_add_header(out, name, value) < 0) {
				r->overflow = true;
			}
		}
		efree(name);
	}
	return SAPI_HEADER_SENT_SUCCESSFULLY;
}

static void fpm_direct_flush(void *context)
{
	(void) context;
	/* Freeze PHP's headers, but never re-enter libevent from PHP: that could
	 * run another request against the same engine globals. Output is buffered. */
	if (fpm_direct_current) {
		sapi_send_headers();
		SG(headers_sent) = true;
	}
}

static ZEND_FUNCTION(fpm_direct_request_headers)
{
	struct evkeyval *kv;
	ZEND_PARSE_PARAMETERS_NONE();
	array_init(return_value);
	if (fpm_direct_current) {
		for (kv = evhttp_request_get_input_headers(fpm_direct_current->http)->tqh_first; kv; kv = kv->next.tqe_next) {
			add_assoc_string(return_value, kv->key, kv->value);
		}
	}
}

static void fpm_direct_install_sapi(void)
{
	const char *names[] = { "getallheaders", "apache_request_headers", NULL };
	const char **name;
	sapi_module.pre_request_init = NULL;
	sapi_module.deactivate = NULL;
	sapi_module.ub_write = fpm_direct_write;
	sapi_module.flush = fpm_direct_flush;
	sapi_module.getenv = fpm_direct_getenv;
	sapi_module.read_post = fpm_direct_read_post;
	sapi_module.read_cookies = fpm_direct_cookies;
	sapi_module.register_server_variables = fpm_direct_register_variables;
	sapi_module.send_headers = fpm_direct_send_headers;
	/* These CGI entry points bypass SAPI callbacks and cast server_context to
	 * fcgi_request. Replace the header readers and remove the finish operation
	 * rather than leave a PHP-callable crash in this otherwise FastCGI-free loop. */
	zend_disable_functions("fastcgi_finish_request");
	for (name = names; *name; name++) {
		zend_function *f = zend_hash_str_find_ptr(CG(function_table), *name, strlen(*name));
		if (f && f->type == ZEND_INTERNAL_FUNCTION) {
			f->internal_function.handler = ZEND_FN(fpm_direct_request_headers);
		}
	}
}

static const char *fpm_direct_method(enum evhttp_cmd_type command)
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

static int fpm_direct_env(struct fpm_direct_request *r, const char *name, const char *value)
{
	return evhttp_add_header(&r->env, name, value ? value : "");
}

static int fpm_direct_prepare_request(struct fpm_direct_worker *w, struct fpm_direct_request *r)
{
	const char *uri = evhttp_request_get_uri(r->http);
	const struct evhttp_uri *parsed = evhttp_request_get_evhttp_uri(r->http);
	const char *method = fpm_direct_method(evhttp_request_get_command(r->http));
	struct evkeyvalq *headers = evhttp_request_get_input_headers(r->http);
	struct evkeyval *kv;
	char length[32], remote_port[16], protocol[32];
	char *peer = NULL;
	ev_uint16_t port;

	/* Origin-form only; the script is selected solely by configuration, never
	 * by the URI, Host, PATH_INFO, or any client-supplied CGI-looking header. */
	if (!uri || uri[0] != '/' || !parsed || !method || evhttp_uri_get_fragment(parsed)) {
		return -1;
	}
	snprintf(length, sizeof(length), "%zu", evbuffer_get_length(evhttp_request_get_input_buffer(r->http)));
	evhttp_connection_get_peer(evhttp_request_get_connection(r->http), &peer, &port);
	snprintf(remote_port, sizeof(remote_port), "%u", (unsigned) port);
	snprintf(protocol, sizeof(protocol), "HTTP/%d.%d", r->http->major, r->http->minor);
#define ENV(key, value) do { if (fpm_direct_env(r, key, value) < 0) return -1; } while (0)
	ENV("REQUEST_METHOD", method);
	ENV("REQUEST_URI", uri);
	ENV("QUERY_STRING", evhttp_uri_get_query(parsed));
	ENV("SCRIPT_FILENAME", w->script);
	ENV("SCRIPT_NAME", w->wp->config->http_front_controller);
	ENV("PHP_SELF", w->wp->config->http_front_controller);
	ENV("PATH_INFO", evhttp_uri_get_path(parsed));
	ENV("DOCUMENT_ROOT", w->root);
	ENV("SERVER_PROTOCOL", protocol);
	ENV("SERVER_SOFTWARE", "php-fpm-ng/http-direct");
	ENV("GATEWAY_INTERFACE", "CGI/1.1");
	ENV("SERVER_ADDR", w->server_addr);
	ENV("SERVER_PORT", w->server_port);
	ENV("SERVER_NAME", evhttp_request_get_host(r->http));
	ENV("REMOTE_ADDR", peer);
	ENV("REMOTE_PORT", remote_port);
	ENV("CONTENT_LENGTH", length);
	ENV("CONTENT_TYPE", evhttp_find_header(headers, "Content-Type"));
	for (kv = headers->tqh_first; kv; kv = kv->next.tqe_next) {
		char name[FPM_DIRECT_HEADERS_MAX + 6];
		size_t i, len = strlen(kv->key);
		if (!strcasecmp(kv->key, "Content-Type") || !strcasecmp(kv->key, "Content-Length") ||
			!strcasecmp(kv->key, "Proxy")) {
			continue;
		}
		if (len > FPM_DIRECT_HEADERS_MAX) {
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

/* Successful completion clears the close callback; a disconnect takes only the
 * close path. Thus draining counts both delivered and abandoned responses. */
static void fpm_direct_response_closed(struct evhttp_connection *connection, void *arg)
{
	struct fpm_direct_worker *w = arg;
	(void) connection;
	w->pending--;
	if (fpm_direct_stopping && !w->pending) event_base_loopbreak(w->base);
}

static void fpm_direct_response_done(struct evhttp_request *request, void *arg)
{
	struct fpm_direct_worker *w = arg;
	evhttp_connection_set_closecb(evhttp_request_get_connection(request), NULL, NULL);
	w->pending--;
	if (fpm_direct_stopping && !w->pending) event_base_loopbreak(w->base);
}

static void fpm_direct_handle(struct evhttp_request *http, void *arg)
{
	struct fpm_direct_worker *w = arg;
	struct fpm_direct_request r = {0};
	zend_file_handle file;
	struct sigaction term_before;
	const char *authorization;

	if (fpm_direct_stopping || w->pending >= FPM_DIRECT_PENDING_MAX) {
		evhttp_add_header(evhttp_request_get_output_headers(http), "Connection", "close");
		evhttp_send_error(http, 503, "Worker unavailable");
		return;
	}
	r.http = http;
	r.status = 200;
	r.env.tqh_last = &r.env.tqh_first;
	r.output = evbuffer_new();
	if (!r.output || fpm_direct_prepare_request(w, &r) < 0) {
		evhttp_clear_headers(&r.env);
		if (r.output) evbuffer_free(r.output);
		evhttp_send_error(http, 400, "Bad request");
		return;
	}

	fpm_direct_current = &r;
	fpm_request_reading_headers(false);
	memset(&SG(request_info), 0, sizeof(SG(request_info)));
	SG(server_context) = &r;
	SG(request_info).path_translated = estrdup(w->script);
	SG(request_info).request_method = fpm_direct_getenv("REQUEST_METHOD", 14);
	SG(request_info).query_string = fpm_direct_getenv("QUERY_STRING", 12);
	SG(request_info).request_uri = fpm_direct_getenv("REQUEST_URI", 11);
	SG(request_info).content_type = fpm_direct_getenv("CONTENT_TYPE", 12);
	SG(request_info).content_length = evbuffer_get_length(evhttp_request_get_input_buffer(http));
	SG(request_info).proto_num = http->major * 1000 + http->minor;
	SG(sapi_headers).http_response_code = 200;
	authorization = evhttp_find_header(evhttp_request_get_input_headers(http), "Authorization");
	php_handle_auth_data(authorization);
	fpm_request_info();

	/* Zend resets SIGTERM during request startup/shutdown (fpm_pool_script.c).
	 * Preserve FPM's immediate termination; SIGQUIT is our graceful flag. */
	sigaction(SIGTERM, NULL, &term_before);
	if (php_request_startup() == FAILURE) {
		zlog(ZLOG_ERROR, "[pool %s] http-direct: PHP request startup failed", w->wp->config->name);
		exit(FPM_EXIT_SOFTWARE);
	}
	sigaction(SIGTERM, &term_before, NULL);
	EG(exit_status) = 0;
	zend_first_try {
		if (fpm_php_limit_extensions(w->script)) {
			SG(sapi_headers).http_response_code = 403;
		} else if (php_fopen_primary_script(&file) == FAILURE) {
			SG(sapi_headers).http_response_code = 404;
		} else {
			fpm_request_executing();
			php_execute_script(&file);
			if (!file.in_list) zend_destroy_file_handle(&file);
		}
	} zend_catch {
		if (!SG(headers_sent)) SG(sapi_headers).http_response_code = 500;
	} zend_end_try();
	fpm_request_end();
	efree(SG(request_info).path_translated);
	SG(request_info).path_translated = NULL;
	php_request_shutdown(NULL);
	sigaction(SIGTERM, &term_before, NULL);
	SG(server_context) = NULL;
	fpm_direct_current = NULL;
	fpm_stdio_flush_child();
	evhttp_clear_headers(&r.env);

	if (r.overflow || r.status < 200 || r.status > 599) {
		evbuffer_drain(r.output, evbuffer_get_length(r.output));
		evhttp_clear_headers(evhttp_request_get_output_headers(http));
		evbuffer_add_printf(r.output, "http-direct: response exceeds POC limits\n");
		r.status = 500;
	}
	w->requests++;
	if (w->wp->config->pm_max_requests && w->requests >= (unsigned) w->wp->config->pm_max_requests) {
		fpm_direct_stopping = 1;
	}
	if (fpm_direct_stopping) {
		evhttp_add_header(evhttp_request_get_output_headers(http), "Connection", "close");
	}
	w->pending++;
	fpm_direct_tick(-1, 0, w);
	evhttp_connection_set_closecb(evhttp_request_get_connection(http), fpm_direct_response_closed, w);
	evhttp_request_set_on_complete_cb(http, fpm_direct_response_done, w);
	evhttp_send_reply(http, r.status, NULL, r.output);
	evbuffer_free(r.output);
	fpm_request_accepting(true);
}

void fpm_http_direct_child_main(struct fpm_worker_pool_s *wp)
{
	struct fpm_direct_worker w = {0};
	struct event *tick;
	struct timeval interval = {0, 10000};
	struct timeval timeout = {wp->config->http_read_timeout / 1000, (wp->config->http_read_timeout % 1000) * 1000};
	struct sigaction action = {0};
	struct stat st;
	struct sockaddr_storage address;
	socklen_t address_len = sizeof(address);
	char candidate[PATH_MAX];

	w.wp = wp;
	if (!getcwd(w.root, sizeof(w.root)) ||
		snprintf(candidate, sizeof(candidate), "%s%s", w.root, wp->config->http_front_controller) >= (int) sizeof(candidate) ||
		!realpath(candidate, w.script) || stat(w.script, &st) < 0 || !S_ISREG(st.st_mode) ||
		(strncmp(w.script, w.root, strlen(w.root)) || (strcmp(w.root, "/") && w.script[strlen(w.root)] != '/'))) {
		zlog(ZLOG_ERROR, "[pool %s] http-direct: front controller must be a regular file inside chdir", wp->config->name);
		exit(FPM_EXIT_CONFIG);
	}
	if (getsockname(wp->listening_socket, (struct sockaddr *) &address, &address_len) == 0) {
		getnameinfo((struct sockaddr *) &address, address_len, w.server_addr, sizeof(w.server_addr),
			w.server_port, sizeof(w.server_port), NI_NUMERICHOST | NI_NUMERICSERV);
	}
	w.base = event_base_new();
	w.http = w.base ? evhttp_new(w.base) : NULL;
	if (!w.http) exit(FPM_EXIT_SOFTWARE);
	evhttp_set_max_headers_size(w.http, FPM_DIRECT_HEADERS_MAX);
	evhttp_set_max_body_size(w.http, wp->config->http_max_body);
	evhttp_set_timeout_tv(w.http, &timeout);
	evhttp_set_allowed_methods(w.http, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD |
		EVHTTP_REQ_PUT | EVHTTP_REQ_DELETE | EVHTTP_REQ_OPTIONS | EVHTTP_REQ_PATCH);
	evhttp_set_gencb(w.http, fpm_direct_handle, &w);
	w.listener = evhttp_accept_socket_with_handle(w.http, wp->listening_socket);
	if (!w.listener) exit(FPM_EXIT_SOFTWARE);
	action.sa_handler = fpm_direct_stop;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGQUIT, &action, NULL) < 0) exit(FPM_EXIT_SOFTWARE);
	fpm_direct_install_sapi();
	fpm_request_accepting(false);
	tick = event_new(w.base, -1, EV_PERSIST, fpm_direct_tick, &w);
	if (!tick || event_add(tick, &interval) < 0) exit(FPM_EXIT_SOFTWARE);
	event_base_dispatch(w.base);
	event_free(tick);
	evhttp_free(w.http);
	event_base_free(w.base);
	exit(FPM_EXIT_OK);
}
