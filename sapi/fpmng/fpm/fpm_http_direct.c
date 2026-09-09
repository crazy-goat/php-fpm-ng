/* Experimental HTTP transport inside a classic FPM worker. The master still
 * owns the children; only their request loop and SAPI I/O change. Unlike the
 * gateway, this event loop cannot progress while PHP is executing. */
#include "fpm_config.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
#include "fpm_http_direct_request.h"
#include "fpm_php.h"
#include "fpm_request.h"
#include "fpm_stdio.h"
#include "zlog.h"

#define FPM_DIRECT_RESPONSE_MAX (8 * 1024 * 1024)
#define FPM_DIRECT_PENDING_MAX 16

const char *const fpm_http_direct_rejects[] = { FPM_HTTP_DIRECT_REJECTS_COMMON, NULL };

/* How this executor names itself in the startup errors of the shared
 * validation (fpm_http_direct_request.c). */
static const struct fpm_http_direct_labels fpm_direct_labels = {
	.subject = "http-direct",
	.chdir_note = "",
	.type_label = "pool.type = http-direct",
	.script_context = "http-direct",
	.script_noun = "front controller",
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
	/* The pool this request belongs to. Only the log prefix needs it, and
	 * fpm_direct_send_headers() reaches the request through fpm_direct_current
	 * without a worker in scope. */
	const char *pool;
	struct evkeyvalq env;
	struct evbuffer *output;
	int status;
	/* Non-NULL once the response cannot go out as the application built it.
	 * Every cause ends the same way — 500, this text as the body — but an
	 * operator reading the wire has to be able to tell them apart. */
	const char *rejected;
};

static struct fpm_direct_request *fpm_direct_current;
static volatile sig_atomic_t fpm_direct_stopping;

int fpm_http_direct_validate(struct fpm_worker_pool_s *wp)
{
	return fpm_http_direct_validate_common(wp, &fpm_direct_labels);
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
	if (!r->rejected && (len > FPM_DIRECT_RESPONSE_MAX - evbuffer_get_length(r->output) ||
		evbuffer_add(r->output, str, len) < 0)) {
		r->rejected = "response body exceeds POC limits";
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
		name = estrndup(h->header, colon - h->header);
		value = colon + 1;
		while (*value == ' ' || *value == '\t') {
			value++;
		}
		if (!strcasecmp(name, "Status")) {
			r->status = atoi(value);
		} else if (!fpm_http_direct_header_dropped(name)) {
			/* Same check as the worker executor, same reason (issue #102):
			 * evhttp_add_header() stores a non-token name verbatim and writes
			 * a malformed header line. header() lets one through — measured on
			 * php-8.5.9: `header(" Lead: ws")` reached the wire as
			 * " Lead: ws", which a proxy may read as a continuation of the
			 * line above, and `header(": novalue")` as ": novalue". Splitting
			 * at the first colon makes an embedded colon unreachable here, but
			 * a space, a tab and an empty name are all reachable.
			 *
			 * A dropped header the application asked for is worse than an
			 * error status — the worker's contract, and the reason this is a
			 * 500 rather than a header quietly missing. */
			if (!fpm_http_direct_header_name_ok(name)) {
				char escaped[256];
				/* The child's zlog fd is closed in fpm_stdio_init_child(), so
				 * this reaches the error log only under
				 * catch_workers_output = yes (issue #73). Still worth writing:
				 * the 500 body deliberately does not echo the name back to
				 * the client, so this is the only place it is recorded. */
				zlog(ZLOG_WARNING, "[pool %s] http-direct: response header name is not "
					"an HTTP token, answering 500: '%s'", r->pool,
					fpm_http_direct_header_name_escape(name, escaped, sizeof(escaped)));
				if (!r->rejected) {
					r->rejected = "malformed response header name";
				}
			} else if (!fpm_http_direct_header_charge(&total, name, strlen(value))) {
				/* First cause wins, as in fpm_direct_write(): the 500 body and
				 * the WARNING above have to name the same trigger, or an
				 * operator chases a limit that was not the one that fired.
				 * Charged here rather than at the top of the loop (issue
				 * #104): a header that is dropped or is the Status:
				 * pseudo-header never reaches the wire, and the worker
				 * executor spends the same budget on the same bytes through
				 * the same call. */
				if (!r->rejected) {
					r->rejected = "response headers exceed POC limits";
				}
				efree(name);
				break;
			} else if (evhttp_add_header(out, name, value) < 0 && !r->rejected) {
				r->rejected = "response header refused by the transport";
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

static int fpm_direct_emit_env(void *ctx, const char *key, const char *value)
{
	struct fpm_direct_request *r = ctx;
	return evhttp_add_header(&r->env, key, value);
}

static int fpm_direct_prepare_request(struct fpm_direct_worker *w, struct fpm_direct_request *r)
{
	const struct fpm_http_direct_env_source source = {
		.script = w->script,
		.root = w->root,
		.front_controller = w->wp->config->http_front_controller,
		.server_addr = w->server_addr,
		.server_port = w->server_port,
		.server_software = "php-fpm-ng/http-direct",
	};

	if (!fpm_http_direct_request_acceptable(r->http)) {
		return -1;
	}
	return fpm_http_direct_build_env(r->http, &source, fpm_direct_emit_env, r);
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
	r.pool = w->wp->config->name;
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
		} else {
			/* php_fopen_primary_script applies doc_root/user_dir to the client
			 * URI. Open our validated path instead: those INI settings must not
			 * bypass the fixed controller or its extension restriction. */
			zend_stream_init_filename(&file, w->script);
			file.primary_script = true;
			if (zend_stream_open(&file) == FAILURE) {
				SG(sapi_headers).http_response_code = 404;
			} else {
				fpm_request_executing();
				php_execute_script(&file);
			}
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

	if (r.rejected || !fpm_http_direct_status_final(r.status)) {
		const char *why = r.rejected ? r.rejected : "response status is not a final status";
		evbuffer_drain(r.output, evbuffer_get_length(r.output));
		evhttp_clear_headers(evhttp_request_get_output_headers(http));
		evbuffer_add_printf(r.output, "http-direct: %s\n", why);
		r.status = 500;
	}
	/* Discards the POC error body above on a HEAD as well: what may carry a
	 * body is a property of the request and the status, not of who produced
	 * the bytes. */
	if (fpm_http_direct_status_bodyless(http, r.status)) {
		evbuffer_drain(r.output, evbuffer_get_length(r.output));
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
	struct sockaddr_storage address;
	socklen_t address_len = sizeof(address);

	w.wp = wp;
	/* Against the directory the child actually chdir'd into, not against the
	 * configured one the master already checked. */
	if (fpm_http_direct_resolve_script(NULL, wp->config->http_front_controller, w.root, w.script) < 0) {
		zlog(ZLOG_ERROR, "[pool %s] %s: %s must be a regular file inside chdir", wp->config->name,
			fpm_direct_labels.script_context, fpm_direct_labels.script_noun);
		exit(FPM_EXIT_CONFIG);
	}
	if (getsockname(wp->listening_socket, (struct sockaddr *) &address, &address_len) == 0) {
		getnameinfo((struct sockaddr *) &address, address_len, w.server_addr, sizeof(w.server_addr),
			w.server_port, sizeof(w.server_port), NI_NUMERICHOST | NI_NUMERICSERV);
	}
	w.base = event_base_new();
	w.http = w.base ? evhttp_new(w.base) : NULL;
	if (!w.http) exit(FPM_EXIT_SOFTWARE);
	evhttp_set_max_headers_size(w.http, FPM_HTTP_HEADERS_MAX);
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
