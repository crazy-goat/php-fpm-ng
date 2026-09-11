/* Experimental HTTP transport inside a classic FPM worker. The master still
 * owns the children; only their request loop and SAPI I/O change. Unlike the
 * gateway, this event loop cannot progress while PHP is executing. */
#include "fpm_config.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/keyvalq_struct.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>

#include "php.h"
#include "php_main.h"
#include "php_variables.h"
#include "fopen_wrappers.h"
#include "SAPI.h"
#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_http_direct.h"
#include "fpm_http_direct_tls.h"
#include "fpm_http_direct_request.h"
#include "fpm_php.h"
#include "fpm_request.h"
#include "fpm_stdio.h"
#include "zlog.h"

#define FPM_DIRECT_RESPONSE_MAX (8 * 1024 * 1024)
#define FPM_DIRECT_PENDING_MAX 16
/* http.stream only. How much output accumulates before it is handed to the
 * transport as one chunk: small enough that a slow consumer sees progress,
 * large enough that a script echoing a few bytes at a time does not pay a
 * chunk header and a write syscall per echo. */
#define FPM_DIRECT_STREAM_CHUNK (64 * 1024)
/* http.stream only. The script keeps running until this much of the response
 * is still unwritten on the connection, then blocks until the client has taken
 * enough of it. This, and not the response size, is the memory a streamed
 * response costs. */
#define FPM_DIRECT_STREAM_HIGHWATER (256 * 1024)

const char *const fpm_http_direct_rejects[] = { FPM_HTTP_DIRECT_REJECTS_COMMON, NULL };

/* How this executor names itself in the startup errors of the shared
 * validation (fpm_http_direct_request.c). */
/* Only this executor has them: the worker executor answers from a PHP callable
 * it drives itself, so there is no per-request SAPI write for a stream to
 * hook. */
static const char *const fpm_direct_extra_directives[] = {
	"http.stream", "http.stream_write_timeout", NULL
};

static const struct fpm_http_direct_labels fpm_direct_labels = {
	.subject = "http-direct",
	.chdir_note = "",
	.type_label = "pool.type = http-direct",
	.script_context = "http-direct",
	.script_noun = "front controller",
	.extra_directives = fpm_direct_extra_directives,
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
	/* Whether this child is inside a request, i.e. whether its event loop is
	 * blocked. Drives the accept gate, see fpm_direct_accept_enable(). */
	int in_request;
};

struct fpm_direct_request {
	/* With http.stream this is cleared the moment the client goes away, which
	 * can now happen while PHP is still producing output, because the pump
	 * below writes to the socket from inside the script. Every streaming path
	 * tests it before touching the transport; NULL means "let the engine run
	 * to a clean shutdown, but throw the bytes away". Without http.stream it
	 * is never NULL. */
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
	/* Everything below is http.stream only; without it they stay zero and
	 * this file behaves exactly as it did before issue #56. */
	struct fpm_direct_worker *w;
	int streaming;		/* the status line and the headers are already on the wire */
	int stream_declined;	/* this response will not stream: decided once, not per write */
	int may_stream;		/* set only around php_execute_script(), see fpm_direct_flush() */
	long stream_waited_ms;	/* http.stream_write_timeout is spent across the whole response */
};

static struct fpm_direct_request *fpm_direct_current;
static void fpm_direct_accept_enable(struct fpm_direct_worker *w, int on);
static volatile sig_atomic_t fpm_direct_stopping;

/* http.stream. Defined below, next to the response-completion callbacks they
 * share with the buffered path; the SAPI hooks just under this are their only
 * callers. */
static void fpm_direct_stream_begin(struct fpm_direct_request *r);
static void fpm_direct_stream_push(struct fpm_direct_request *r);
static void fpm_direct_stream_abort(struct fpm_direct_request *r, const char *why);

int fpm_http_direct_validate(struct fpm_worker_pool_s *wp)
{
	/* Before the shared validation: these two say that a combination cannot
	 * work at all, which is more use to an operator than whatever the generic
	 * check would have complained about first. */
	if (wp->config->http_stream && wp->config->http_stream_write_timeout <= 0) {
		/* A worker that waits forever for one client that stopped reading
		 * serves nobody else: there is exactly one request in flight per child
		 * here. */
		zlog(ZLOG_ALERT, "[pool %s] http.stream requires http.stream_write_timeout > 0", wp->config->name);
		return -1;
	}
	/* The pump writes the connection's output buffer to its own descriptor,
	 * which is only the response for as long as the two carry the same bytes.
	 * On a TLS connection that buffer holds plaintext, and libevent 2.1 has no
	 * way to flush an SSL bufferevent from inside a callback
	 * (be_openssl_flush() is an "XXXX Implement this" stub), so there is no
	 * correct implementation to fall back to. Refusing the combination beats a
	 * directive that quietly does nothing on the pools that most want it. */
	if (wp->config->http_stream && wp->config->http_tls_cert && *wp->config->http_tls_cert) {
		zlog(ZLOG_ALERT, "[pool %s] http.stream cannot be combined with http.tls_cert: the streaming "
			"writer cannot flush an encrypted connection from inside a request", wp->config->name);
		return -1;
	}
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
		return;
	}
	/* The safety net for the accept gate, not its normal path: the end of a
	 * request re-opens accepting itself, through the call to this function that
	 * fpm_direct_handle() makes before it returns. What is left for the timer is
	 * the child that accepted a connection and was then told nothing -- no
	 * request, so no end of one -- which would otherwise sit out of accept until
	 * the read timeout closed the silent connection. */
	if (!w->in_request) {
		fpm_direct_accept_enable(w, 1);
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
	if (!fpm_direct_current->http) {
		return NULL;	/* http.stream: the client went away mid-request */
	}
	return (char *) evhttp_find_header(evhttp_request_get_input_headers(fpm_direct_current->http), "Cookie");
}

static size_t fpm_direct_read_post(char *buffer, size_t size)
{
	int n;
	/* php_request_shutdown() drains whatever the script did not read, long
	 * after a streamed response may have lost its client. Measured on the test
	 * box against the first version of this file: SIGSEGV inside
	 * evhttp_request_get_input_buffer(NULL), from sapi_deactivate_module(). */
	if (!fpm_direct_current->http) {
		return 0;
	}
	n = evbuffer_remove(evhttp_request_get_input_buffer(fpm_direct_current->http), buffer, size);
	return n < 0 ? 0 : (size_t) n;
}

static size_t fpm_direct_write(const char *str, size_t len)
{
	struct fpm_direct_request *r = fpm_direct_current;
	if (!r) {
		return 0;
	}
	/* An opted-in pool leaves the buffered path here, one chunk short of
	 * filling it -- before FPM_DIRECT_RESPONSE_MAX can reject the response,
	 * which is the whole point of issue #56. */
	if (!r->streaming && r->may_stream &&
		len > FPM_DIRECT_STREAM_CHUNK - evbuffer_get_length(r->output)) {
		fpm_direct_stream_begin(r);
	}
	if (r->streaming) {
		size_t done = 0;

		/* One chunk at a time, not one SAPI write at a time. `len` is whatever
		 * the script handed to echo -- with output_buffering = 0 a single
		 * `echo file_get_contents($big)` is one write -- and buffering all of
		 * it before the first push would make the peak the size of that write
		 * rather than FPM_DIRECT_STREAM_HIGHWATER. */
		while (done < len) {
			size_t take = len - done;

			/* r->http == NULL means the client is already gone: the bytes go
			 * nowhere, but PHP must still reach its own shutdown, so the
			 * buffer is simply not grown. */
			if (!r->http) {
				break;
			}
			if (take > FPM_DIRECT_STREAM_CHUNK - evbuffer_get_length(r->output)) {
				take = FPM_DIRECT_STREAM_CHUNK - evbuffer_get_length(r->output);
			}
			if (evbuffer_add(r->output, str + done, take) < 0) {
				fpm_direct_stream_abort(r, "out of memory while buffering a chunk");
				break;
			}
			done += take;
			if (evbuffer_get_length(r->output) >= FPM_DIRECT_STREAM_CHUNK) {
				fpm_direct_stream_push(r);
			}
		}
		return len;
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
	struct evkeyvalq *out;
	sapi_header_struct *h;
	zend_llist_position pos;
	size_t total = 0;

	r->status = headers->http_response_code;
	/* http.stream: nowhere to put them, and on a streamed response they went
	 * out before the client left anyway. */
	if (!r->http) {
		return SAPI_HEADER_SENT_SUCCESSFULLY;
	}
	out = evhttp_request_get_output_headers(r->http);
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
	struct fpm_direct_request *r = fpm_direct_current;
	(void) context;
	if (!r) {
		return;
	}
	/* Freeze PHP's headers. Without http.stream that is all a flush can do:
	 * re-entering libevent from PHP could run another request against the same
	 * engine globals, so the output stays buffered. http.stream pushes it
	 * instead -- without re-entering libevent either, see the pump below. */
	sapi_send_headers();
	SG(headers_sent) = true;
	/* Only from the script itself. php_request_shutdown() flushes too, and
	 * turning every ordinary response chunked at that point would cost each
	 * one its Content-Length for nothing. */
	if (r->may_stream) {
		fpm_direct_stream_begin(r);
		if (r->streaming) {
			fpm_direct_stream_push(r);
		}
	}
}

static ZEND_FUNCTION(fpm_direct_request_headers)
{
	struct evkeyval *kv;
	ZEND_PARSE_PARAMETERS_NONE();
	array_init(return_value);
	if (fpm_direct_current && fpm_direct_current->http) {
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
		.tls = fpm_http_direct_tls_enabled(w->wp),
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

/* --- http.stream ---------------------------------------------------------
 *
 * The buffered path above builds the whole response in r->output and hands it
 * to libevent once, after PHP has shut down. That caps a response at
 * FPM_DIRECT_RESPONSE_MAX and delays the first byte until the last one exists.
 *
 * Streaming lifts both, and the price is that bytes have to reach the socket
 * while PHP is still on the stack -- and this child's event loop cannot run
 * then. The comment that used to sit in fpm_direct_flush() gave one reason;
 * libevent enforces another anyway. Measured on the test box against the first
 * version of this file (libevent 2.1.12): a nested event_base_loop() on the
 * base we are already dispatching from prints "event_base_loop: reentrant
 * invocation" and returns -1, and every streamed response came out truncated.
 *
 * So the pump never runs the loop. It writes the connection's own output
 * buffer to its own descriptor with poll() and evbuffer_write(), which is what
 * bufferevent_writecb() would have done, and leaves the rest of the child
 * untouched: no other connection is read, no request is dispatched, nothing
 * can execute PHP inside PHP. When the request callback finally returns, the
 * write event is still armed and libevent finishes the reply its usual way.
 *
 * The cost is that this reaches past the bufferevent to the descriptor, which
 * is only correct while the two carry the same bytes. They do not on a TLS
 * connection -- that output buffer holds plaintext -- and libevent 2.1 offers
 * nothing to flush an SSL bufferevent from inside a callback
 * (be_openssl_flush() is an "XXXX Implement this" stub). http.stream therefore
 * refuses to start on a pool with http.tls_cert; see fpm_http_direct_validate().
 */

/* The close callback of a response that is already on the wire. Unlike the
 * buffered path, the request object can be freed by libevent while PHP is
 * still producing output for it, so this clears the pointer the SAPI hooks
 * test rather than only counting. */
static void fpm_direct_stream_closed(struct evhttp_connection *connection, void *arg)
{
	struct fpm_direct_request *r = arg;
	(void) connection;
	r->http = NULL;
	r->w->pending--;
	if (fpm_direct_stopping && !r->w->pending) event_base_loopbreak(r->w->base);
}

/* Whether this response is the last one this child will serve, decided before
 * the headers go out because a streamed response cannot gain a Connection
 * header afterwards. Mirrors the two triggers the buffered tail applies after
 * w->requests++.
 *
 * The known gap: a SIGQUIT that arrives while the script is still producing
 * output sets fpm_direct_stopping too late to be answered with
 * Connection: close, so a client on that one connection learns the child is
 * gone from the close rather than from the header. The buffered path has the
 * whole response in hand when it decides and does not. Documented in
 * docs/http-direct.md rather than papered over: the header cannot be recalled
 * once it is on the wire. */
static bool fpm_direct_last_request(const struct fpm_direct_worker *w)
{
	int max = w->wp->config->pm_max_requests;
	return fpm_direct_stopping || (max > 0 && w->requests + 1 >= (unsigned) max);
}

static void fpm_direct_stream_begin(struct fpm_direct_request *r)
{
	struct fpm_direct_worker *w = r->w;

	if (r->streaming || r->stream_declined) {
		return;
	}
	/* Every reason to stay buffered, and each one matters:
	 *  - the pool did not ask for streaming;
	 *  - the client is already gone;
	 *  - the response is doomed already, and the buffered tail still owes it a
	 *    500 whose body names the cause -- impossible once a status line is on
	 *    the wire;
	 *  - the status is not one evhttp_send_reply() would frame, which the
	 *    buffered tail also refuses to send;
	 *  - HEAD/204/205/304 carry no body, so there is nothing to stream and the
	 *    buffered path already drops the bytes;
	 *  - the client speaks HTTP/1.0, which has no chunked framing: libevent
	 *    would answer Content-Length: 0 (the buffer is empty when the headers
	 *    go out) and then write the body after it, so a keep-alive client
	 *    would read the body as the start of the next response.
	 */
	if (!w->wp->config->http_stream || !r->http || r->rejected ||
		r->http->major != 1 || r->http->minor < 1 ||
		!fpm_http_direct_status_final(r->status) ||
		fpm_http_direct_status_bodyless(r->http, r->status)) {
		r->stream_declined = 1;
		return;
	}
	if (fpm_direct_last_request(w)) {
		evhttp_add_header(evhttp_request_get_output_headers(r->http), "Connection", "close");
	}
	/* Counted from here rather than from the tail: from this point a client
	 * that disappears has to be noticed, and w->pending is what the shutdown
	 * path waits on. */
	w->pending++;
	evhttp_connection_set_closecb(evhttp_request_get_connection(r->http), fpm_direct_stream_closed, r);
	evhttp_request_set_on_complete_cb(r->http, fpm_direct_response_done, w);
	/* libevent picks chunked itself for an HTTP/1.1 client with no
	 * Content-Length, and omits the framing entirely for a bodyless response. */
	evhttp_send_reply_start(r->http, r->status, NULL);
	r->streaming = 1;
}

/* Takes the connection away from the client without pretending the message
 * ended. A chunked body cut short of its terminator is the only truthful thing
 * left to send once the status line has gone out; shutdown() is how the
 * connection dies from inside a callback, because the event loop then sees the
 * socket fail on its next pass and frees the request there, not on this stack. */
static void fpm_direct_stream_abort(struct fpm_direct_request *r, const char *why)
{
	struct evhttp_connection *connection;
	struct bufferevent *bev;
	evutil_socket_t fd;

	if (!r->http) {
		return;
	}
	zlog(ZLOG_WARNING, "[pool %s] http-direct: %s; the streamed response is cut short and the "
		"connection closed without its terminating chunk", r->pool, why);
	connection = evhttp_request_get_connection(r->http);
	/* Both callbacks go first: this function does the accounting itself, and
	 * the request is about to be freed by somebody else. */
	evhttp_request_set_on_complete_cb(r->http, NULL, NULL);
	evhttp_connection_set_closecb(connection, NULL, NULL);
	r->http = NULL;
	r->w->pending--;
	evbuffer_drain(r->output, evbuffer_get_length(r->output));
	bev = evhttp_connection_get_bufferevent(connection);
	fd = bev ? bufferevent_getfd(bev) : -1;
	if (fd >= 0) {
		shutdown(fd, SHUT_RDWR);
	}
}

/* Milliseconds since `start`, or -1 if the clock is unreadable -- in which case
 * the caller must not wait at all rather than wait unbounded. */
static long fpm_direct_stream_elapsed(const struct timespec *start)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return -1;
	}
	return (long) (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

/* Writes as much of the response as the socket will take right now, and then
 * blocks the script until the client has taken enough of the rest to leave
 * less than `limit` unwritten.
 *
 * The first half is what makes flush() mean something: the bytes are on the
 * wire when the script asked for them to be, not when the request callback
 * returns. The second half is the backpressure -- without it the connection's
 * output buffer would grow with the response and streaming would have traded
 * one unbounded buffer for another.
 *
 * The wait is bounded by http.stream_write_timeout, because there is exactly
 * one request in flight per child here: a client that stops reading must not
 * take the worker with it. The budget is spent across the whole response
 * (r->stream_waited_ms), not restarted per call: a client that takes one byte
 * every timeout-minus-one milliseconds makes progress every time and would
 * otherwise hold the worker for as long as it cared to. Only time the script
 * spends *blocked* counts, so a client keeping up pays nothing. */
static void fpm_direct_stream_pump(struct fpm_direct_request *r, size_t limit)
{
	struct bufferevent *bev;
	struct evbuffer *out;
	evutil_socket_t fd;
	int budget = r->w->wp->config->http_stream_write_timeout;
	int idle_writes = 0;

	if (!r->http) {
		return;
	}
	bev = evhttp_connection_get_bufferevent(evhttp_request_get_connection(r->http));
	fd = bev ? bufferevent_getfd(bev) : -1;
	if (!bev || fd < 0) {
		return;
	}
	out = bufferevent_get_output(bev);
	while (evbuffer_get_length(out) > 0) {
		struct pollfd pfd = { fd, POLLOUT, 0 };
		struct timespec before;
		long waited;
		int left, ready, written;

		/* Exactly what bufferevent_writecb() does with this buffer and this
		 * descriptor, minus the event loop we are not allowed to re-enter --
		 * including the unfreeze. A bufferevent keeps the front of its output
		 * buffer frozen so that nothing but its own writer may drain it, and
		 * evbuffer_write() on a frozen buffer fails without touching errno:
		 * measured on the test box against the first version of this file,
		 * where every write "failed", the response went out only when the
		 * callback returned, and the stale errno eventually looked fatal and
		 * aborted the connection. errno is cleared for the same reason. */
		errno = 0;
		evbuffer_unfreeze(out, 1);
		written = evbuffer_write(out, fd);
		evbuffer_freeze(out, 1);
		if (written > 0) {
			idle_writes = 0;
			continue;
		}
		if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			fpm_direct_stream_abort(r, "writing the response to the client failed");
			return;
		}
		/* A zero-length write on a non-empty buffer means neither progress nor
		 * an error to act on. Once in a row is tolerated; twice, with poll()
		 * still calling the descriptor writable in between, would be a loop
		 * spinning at full CPU until the budget ran out. */
		if (written == 0 && ++idle_writes > 1) {
			fpm_direct_stream_abort(r, "the client connection accepted no data");
			return;
		}
		/* The socket is full. Below the mark that is fine: the rest goes out
		 * through libevent once this request callback returns. */
		if (evbuffer_get_length(out) <= limit) {
			return;
		}
		left = (int) (budget - r->stream_waited_ms);
		if (left <= 0 || clock_gettime(CLOCK_MONOTONIC, &before) != 0) {
			/* No budget, or no clock to bound the next wait with. Either way
			 * blocking again is what must not happen. */
			fpm_direct_stream_abort(r, "the client stopped reading the response");
			return;
		}
		ready = poll(&pfd, 1, left);
		waited = fpm_direct_stream_elapsed(&before);
		r->stream_waited_ms += waited < 0 ? left : waited;
		if (ready < 0 && errno != EINTR) {
			fpm_direct_stream_abort(r, "poll() on the client connection failed");
			return;
		}
		if (ready > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
			fpm_direct_stream_abort(r, "the client closed the connection mid-response");
			return;
		}
	}
}

static void fpm_direct_stream_push(struct fpm_direct_request *r)
{
	if (!r->http) {
		evbuffer_drain(r->output, evbuffer_get_length(r->output));
		return;
	}
	if (evbuffer_get_length(r->output)) {
		/* Drains r->output into the connection: no copy, and no part of the
		 * response is held twice. */
		evhttp_send_reply_chunk(r->http, r->output);
	}
	fpm_direct_stream_pump(r, FPM_DIRECT_STREAM_HIGHWATER);
}

/* The streaming counterpart of the buffered tail of fpm_direct_handle(). */
static void fpm_direct_stream_finish(struct fpm_direct_request *r)
{
	if (!r->http) {
		return;
	}
	if (r->rejected) {
		/* The buffered path answers 500 with the cause as the body. Here the
		 * status line left the process before the cause was known, so the only
		 * signal left is an unterminated message. */
		fpm_direct_stream_abort(r, r->rejected);
		return;
	}
	if (evbuffer_get_length(r->output)) {
		evhttp_send_reply_chunk(r->http, r->output);
	}
	/* Hand the connection back to the callback the buffered path uses. `r` is
	 * a local of fpm_direct_handle() and stops existing the moment this
	 * request is over, while the close notice can arrive much later -- on the
	 * next keep-alive idle close. Measured on the test box against the first
	 * version of this file: a second request on the same connection, then
	 * SIGSEGV in the close callback reading through the dead frame. */
	evhttp_connection_set_closecb(evhttp_request_get_connection(r->http),
		fpm_direct_response_closed, r->w);
	/* No pump here: from this point the response is an ordinary queued reply,
	 * w->pending keeps the child alive for it, and the event loop writes it out
	 * exactly as it does a buffered one. */
	evhttp_send_reply_end(r->http);
}

/* THE ACCEPT GATE (issue #53)
 *
 * Every child calls evhttp_accept_socket_with_handle() on the one listening
 * socket the master opened, so which child serves a connection is decided by
 * whichever one the kernel wakes. That much would be fair. What is not fair is
 * libevent's accept loop: listener_read_cb() accept()s until the queue is
 * empty, so a child that wakes first scoops an entire burst into its own
 * connection list and then serves it one request at a time -- with the event
 * loop blocked for the whole of each -- while its siblings sit idle. Measured
 * on a 4-worker pool before this gate: a keep-alive run put every connection on
 * one child, and one slow request made the seven fast ones queued behind it
 * cost 1005 ms each instead of ~10 ms.
 *
 * The fix is to stop the drain after one connection. listener_read_cb()
 * re-checks lev->enabled after each callback, so a callback that disables the
 * listener ends the loop there and leaves the rest of the queue for whichever
 * child wakes next. evhttp's only per-accepted-connection hook is the
 * bufferevent callback, which is why the gate rides on evhttp_set_bevcb().
 *
 * An earlier attempt that closed the gate only while PHP ran measured as noise,
 * for a reason the numbers made obvious: in a burst every child is idle, so the
 * accept storm is over before any PHP starts and the window it closed was never
 * open.
 *
 * On a TLS pool the bevcb belongs to fpm_http_direct_tls.c, which owns the
 * SSL_CTX and reinstalls the pair whenever the certificate is reloaded. There
 * the gate is handed to it as an on-accept hook rather than registered here, so
 * that a reload cannot quietly drop it. */
static void fpm_direct_accept_enable(struct fpm_direct_worker *w, int on)
{
	struct evconnlistener *l = w->listener ? evhttp_bound_socket_get_listener(w->listener) : NULL;

	if (!l) {
		return;
	}
	if (on) {
		evconnlistener_enable(l);
	} else {
		evconnlistener_disable(l);
	}
}

/* Shared by the plain bevcb below and, through
 * fpm_http_direct_tls_child_attach(), by the TLS one. */
static void fpm_direct_accept_close_gate(void *arg)
{
	fpm_direct_accept_enable(arg, 0);
}

static struct bufferevent *fpm_direct_accept_bevcb(struct event_base *base, void *arg)
{
	fpm_direct_accept_close_gate(arg);
	return bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
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
	r.w = w;
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

	w->in_request = 1;
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
				/* The only window in which a flush() may put the response on
				 * the wire: see fpm_direct_flush(). */
				r.may_stream = 1;
				php_execute_script(&file);
				r.may_stream = 0;
			}
			if (!file.in_list) zend_destroy_file_handle(&file);
		}
	} zend_catch {
		r.may_stream = 0;
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

	if (r.streaming) {
		/* The status line and the headers are long gone; everything the
		 * buffered tail below decides was decided in fpm_direct_stream_begin(),
		 * before the first byte left. */
		w->requests++;
		if (w->wp->config->pm_max_requests && w->requests >= (unsigned) w->wp->config->pm_max_requests) {
			fpm_direct_stopping = 1;
		}
		fpm_direct_stream_finish(&r);
		evbuffer_free(r.output);
		w->in_request = 0;
		fpm_direct_tick(-1, 0, w);
		fpm_request_accepting(true);
		return;
	}
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
	w->in_request = 0;
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
	/* Before the listener is attached, so the first connection this child
	 * accepts is already on this process's own SSL_CTX. A no-op on a pool
	 * without http.tls_cert. */
	if (fpm_http_direct_tls_child_attach(wp, w.base, w.http, fpm_direct_accept_close_gate, &w) < 0) {
		exit(FPM_EXIT_SOFTWARE);
	}
	/* The accept gate. On a TLS pool it went in as the hook above, because there
	 * the bevcb is the TLS one. */
	if (!fpm_http_direct_tls_enabled(wp)) {
		evhttp_set_bevcb(w.http, fpm_direct_accept_bevcb, &w);
	}
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
