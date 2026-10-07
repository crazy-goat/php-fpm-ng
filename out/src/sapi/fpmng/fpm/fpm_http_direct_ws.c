/* fpm-ng: WebSocket hijack for the worker executor (issue #343), split out of
 * fpm_http_direct_worker.c (#747). See fpm_http_direct_worker.c for the
 * executor itself; the shared state is in fpm_http_direct_worker_internal.h. */
#include "fpm_config.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/keyvalq_struct.h>
#include <event2/buffer.h>
#ifndef TAILQ_FIRST
#define TAILQ_FIRST(head) ((head)->tqh_first)
#endif
#ifndef TAILQ_NEXT
#define TAILQ_NEXT(elm, field) ((elm)->field.tqe_next)
#endif
#ifndef TAILQ_FOREACH
#define TAILQ_FOREACH(var, head, field) \
	for ((var) = TAILQ_FIRST(head); (var); (var) = TAILQ_NEXT(var, field))
#endif
#include <event2/bufferevent.h>
#ifdef HAVE_FPM_HTTP_TLS
#include <event2/bufferevent_ssl.h>
#include <openssl/ssl.h>
#endif

#include "php.h"
#include "php_streams.h"
#include "zend_smart_str.h"
/* issue #343: Sec-WebSocket-Accept is base64(SHA1(key || GUID)) -- both
 * primitives are PHPAPI in ext/standard, which is always linked. */
#include "ext/standard/base64.h"
#include "ext/standard/sha1.h"
#include "zend_API.h"
#include "zend_exceptions.h"
#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_http_direct_request.h"
#include "fpm_http_direct_worker.h"
#include "fpm_http_direct_worker_internal.h"
#include "fpm_http_direct_conn.h"
#include "fpm_http_direct_tls.h"
#include "zlog.h"

/* ------------------------------------------------------------------ issue #343 */

/* fpmng_worker_upgrade(): the one builtin of the WebSocket story. The worker
 * executor runs PHP in the process that owns the accepted connection's
 * bufferevent, so a connection can leave evhttp's request/response world and
 * become an ordinary bidirectional PHP stream -- RFC 6455 framing, masking,
 * ping/pong and close codes stay in userland codecs (amphp/websocket-server,
 * ratchet/rfc6455, ReactPHP), exactly as this executor keeps Revolt/amphp
 * out of C. The classic executor keeps all three of #68's impossibility
 * policies: it has no event loop of its own to hand a hijacked connection to.
 *
 * The hijack follows libevent 2.2's own evws_new_session() (ws.c) in intent,
 * on the public 2.1 API -- and where 2.2's internal evhttp_start_ws_() steals
 * evcon->bufev and frees the connection, the public API can reach neither
 * evcon->bufev nor the connection list, and 2.1's own
 * evhttp_connection_free() shutdown(SHUT_WR)s the fd on the way out, which
 * would kill the WebSocket the moment it was born. So the connection stays
 * evhttp's for the child's whole life: the hijack writes the 101 into the
 * bufferevent, replaces ALL of its callbacks (evhttp's state machine on this
 * connection is never driven again), clears the closecb and the timeouts, and
 * leaves the request exactly where evhttp put it -- never answered, never
 * driven, freed with the connection at evhttp_free(). The stream's close drains
 * queued output, performs TLS shutdown when the bufferevent carries SSL*, and
 * closes the fd; the registry below (fpm_ws_all, walked in child_main() just
 * before evhttp_free()) tells a late
 * stream close the bufferevent is already gone. A closed stream releases its
 * connection (fpm_ws_release(), #472) once the close tail is done; before
 * that, every closed stream kept its fd, bufferevent and request until the
 * worker recycled. Only a connection still open at teardown is left to
 * evhttp_free().
 *
 * Readiness: an fd watcher alone cannot see this connection's data -- the
 * bufferevent drains the descriptor into its input evbuffer, and a drained
 * descriptor never fires. The bufferevent's own read/write callbacks
 * therefore activate the read/write watchers bound to this context
 * (watcher->ws, wired in fpmng_worker_event_create()), which is also what
 * makes fpmng_worker_stream_has_buffered() and feof() honest: fpmng stream
 * ops answer from the evbuffers, and the event callback flags EOF on the
 * stream the moment the peer goes away. */

/* Every connection this child has hijacked and not yet closed, so the
 * teardown in child_main() can tell the stream close ops that evhttp_free()
 * is about to free the bufferevents it never let go of. The head is touched
 * only from this child's own single-threaded callbacks. */
struct fpm_ws_ctx *fpm_ws_all = NULL;

/* issue #458: a nonblocking SSL_shutdown() may have written only part of
 * close_notify. This state owns the retry event, never the bufferevent or
 * SSL*: evhttp keeps owning both until child_main's teardown. Every node here
 * contributes one fw.ws_unflushed close the bounded output flush must wait for. */
struct fpm_ws_tls_close {
	struct bufferevent *bev;
	struct evhttp_connection *release; /* #472: freed once the close completes; NULL = left to evhttp_free() */
	struct event *event;
	struct fpm_ws_tls_close *next;
};

static struct fpm_ws_tls_close *fpm_ws_tls_closing = NULL;

static void fpm_ws_finish_close(struct bufferevent *bev, bool counted, bool transport_error,
		struct evhttp_connection *release);

static void fpm_ws_unregister(struct fpm_ws_ctx *ctx)
{
	struct fpm_ws_ctx **p = &fpm_ws_all;

	while (*p && *p != ctx) {
		p = &(*p)->next;
	}
	if (*p) {
		*p = ctx->next;
	}
}

void fpm_ws_unregister_watcher(struct fpm_worker_watcher *watcher)
{
	struct fpm_ws_ctx *ctx = watcher->ws;
	unsigned i;

	watcher->ws = NULL;
	if (!ctx) {
		return;
	}
	for (i = 0; i < ctx->watchers_n; i++) {
		if (ctx->watchers[i] == watcher->id) {
			ctx->watchers[i] = ctx->watchers[--ctx->watchers_n];
			return;
		}
	}
}

/* The watcher callbacks run PHP, so every fire needs the same guard
 * fpm_worker_activate_buffered() and fpm_worker_watcher_fire() use: the
 * callback may free watchers (event_free inside a callback is the advertised
 * cancellation idiom), so ids are snapshotted and each is re-looked-up and
 * re-checked before event_active() -- which only arms the event for THIS
 * iteration, exactly what a real fd readability would have done. */
static void fpm_ws_fire(struct fpm_ws_ctx *ctx, short type)
{
	zend_ulong *ids;
	unsigned n = ctx->watchers_n, i;

	if (!n) {
		return;
	}
	ids = emalloc(n * sizeof(*ids));
	memcpy(ids, ctx->watchers, n * sizeof(*ids));
	for (i = 0; i < n; i++) {
		struct fpm_worker_watcher *watcher = zend_hash_index_find_ptr(&fw.watchers, ids[i]);

		if (!watcher || watcher->ws != ctx || watcher->type != type || !event_pending(watcher->ev, type == FPM_WORKER_EV_READ ? EV_READ : EV_WRITE, NULL)) {
			continue;
		}
		event_active(watcher->ev, type == FPM_WORKER_EV_READ ? EV_READ : EV_WRITE, 0);
	}
	efree(ids);
}

static void fpm_ws_readcb(struct bufferevent *bev, void *arg)
{
	(void) bev;
	fpm_ws_fire(arg, FPM_WORKER_EV_READ);
}

static void fpm_ws_writecb(struct bufferevent *bev, void *arg)
{
	(void) bev;
	fpm_ws_fire(arg, FPM_WORKER_EV_WRITE);
}

/* Issue #460: an EOF'd descriptor is permanently readable, so a level-triggered
 * EV_PERSIST read watcher bound to it (fpmng_worker_event_create() binds read
 * watchers to the stream's descriptor) fires on every event-loop iteration
 * forever -- thousands of empty wakeups at 100% CPU until the stream is
 * closed. Deliver the EOF wakeup to every read watcher on this connection ONCE
 * and then take each off its descriptor. The input evbuffer is left untouched,
 * so fread() still drains whatever arrived with the FIN (that is issue #456's
 * half of the contract); only the level-triggered re-fire loop ends.
 *
 * event_del() then event_active(), in that order: event_del() would cancel a
 * pending activation, while event_active() works on an event that is no longer
 * pending -- it runs once and is not rescheduled. */
static void fpm_ws_eof_wakeup_once(struct fpm_ws_ctx *ctx)
{
	zend_ulong *ids;
	unsigned n = ctx->watchers_n, i;

	if (!n) {
		return;
	}
	ids = emalloc(n * sizeof(*ids));
	memcpy(ids, ctx->watchers, n * sizeof(*ids));
	for (i = 0; i < n; i++) {
		struct fpm_worker_watcher *watcher = zend_hash_index_find_ptr(&fw.watchers, ids[i]);

		if (!watcher || watcher->ws != ctx || watcher->type != FPM_WORKER_EV_READ) {
			continue;
		}
		event_del(watcher->ev);
		event_active(watcher->ev, EV_READ, 0);
	}
	efree(ids);
}

static void fpm_ws_eventcb(struct bufferevent *bev, short what, void *arg)
{
	struct fpm_ws_ctx *ctx = arg;

	if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
		ctx->eof = true;
		/* The userspace answer to "did the peer go away": the next fread()
		 * comes up empty and feof() is true. Setting the flag on the stream
		 * here (rather than in the read op) is what makes feof() exact -- a
		 * merely empty buffer must not read as EOF. */
		if (ctx->stream) {
			ctx->stream->eof = 1;
		}
		/* Stop the bufferevent reading this fd too (issue #460): fpm_ws_read()
		 * answers from the input evbuffer, so disabling EV_READ here does not
		 * hide any buffered byte, it only stops readcb() from re-firing on the
		 * EOF-readable descriptor. */
		bufferevent_disable(bev, EV_READ);
		fpm_ws_eof_wakeup_once(ctx);
	}
}

static ssize_t fpm_ws_read(php_stream *stream, char *buf, size_t count)
{
	struct fpm_ws_ctx *ctx = stream->abstract;
	struct evbuffer *in;
	size_t buffered;
	size_t take;

	if (ctx->orphaned) {
		/* Teardown already freed the bufferevent (#443): answer like a dead
		 * stream, never touch it. */
		return 0;
	}
	in = bufferevent_get_input(ctx->bev);
	buffered = evbuffer_get_length(in);

	if (count > buffered) {
		count = buffered;
	}
	/* 0 = "nothing buffered" -- _php_stream_read() does not treat a 0 from
	 * the ops read as EOF, so feof() stays exactly as the event callback
	 * flags it. An empty buffer on this stream is the NORMAL state between
	 * WebSocket messages, not an error and not an end. */
	take = count ? (size_t) evbuffer_remove(in, buf, count) : 0;
	return (ssize_t) take;
}

static ssize_t fpm_ws_write(php_stream *stream, const char *buf, size_t count)
{
	struct fpm_ws_ctx *ctx = stream->abstract;
	struct evbuffer *out;

	if (ctx->orphaned) {
		/* The bufferevent is gone (#443): report the write failed rather
		 * than queue bytes into freed heap. */
		return -1;
	}
	out = bufferevent_get_output(ctx->bev);

	/* #332's backpressure contract, on the connection's own output evbuffer:
	 * once what is queued-but-unwritten is at the limit, refuse to queue more
	 * and return 0 -- the write watcher (fired by fpm_ws_writecb() when the
	 * buffer drains) is what wakes the codec to try again. */
	if (fw.wp->config->worker_send_buffer_limit > 0 && evbuffer_get_length(out) >= (size_t) fw.wp->config->worker_send_buffer_limit) {
		return 0;
	}
	return bufferevent_write(ctx->bev, buf, count) == 0 ? (ssize_t) count : -1;
}

static void fpm_ws_close_after_write(struct bufferevent *bev, short what, void *arg);
static void fpm_ws_close_after_write_cb(struct bufferevent *bev, void *arg);

static void fpm_ws_log_failed_upgrade(struct fpm_ws_ctx *ctx, int ws_fd, size_t queued)
{
	if (ctx->failure_logged) {
		return;
	}
	ctx->failure_logged = true;
	zlog(ZLOG_WARNING, "[pool %s] http-direct worker: fpmng_worker_upgrade() request id=%lu "
					   "method=GET uri=%s was followed by a userland exception; outcome=%s queued_bytes=%zu",
			fw.wp->config->name, (unsigned long) ctx->request_id,
			ctx->request_uri ? ctx->request_uri : "(unknown)",
			ws_fd >= 0 && queued > 0 ? "close_after_write" : "close", queued);
}

static void fpm_ws_start_close_after_write(struct fpm_ws_ctx *ctx, bool release)
{
	if (ctx->close_after_write) {
		return;
	}
	ctx->close_after_write = true;
	fw.ws_unflushed++;
	bufferevent_setcb(ctx->bev, NULL, fpm_ws_close_after_write_cb, fpm_ws_close_after_write,
			release ? ctx->evcon : NULL);
	bufferevent_enable(ctx->bev, EV_WRITE);
}

static void fpm_ws_shutdown_now(struct fpm_ws_ctx *ctx, bool release)
{
	/* Disarm before shutdown: the context may be freed immediately after this
	 * helper returns, while evhttp still owns the bufferevent. */
	bufferevent_setcb(ctx->bev, NULL, NULL, NULL, NULL);
	bufferevent_disable(ctx->bev, EV_READ | EV_WRITE);
	fpm_ws_finish_close(ctx->bev, false, false, release ? ctx->evcon : NULL);
	ctx->eof = true;
}

static int fpm_ws_close(php_stream *stream, int close_handle)
{
	struct fpm_ws_ctx *ctx = stream->abstract;
	zend_ulong *ids;
	unsigned n, i;

	(void) close_handle;
	fpm_ws_unregister(ctx);
	/* The connection is going away under the watchers watching it: drop them
	 * (their dtor runs fpm_ws_unregister_watcher(), so the array empties
	 * through the same path userland's event_free() uses). Snapshot ids for
	 * the same reason every other walk here does. */
	n = ctx->watchers_n;
	/* safe_emalloc, no NULL branch: it aborts on OOM (the same shape
	 * fpm_worker_activate_buffered()'s id snapshot uses), so the loop below
	 * has no null to dereference whatever n is. */
	ids = safe_emalloc(n, sizeof(*ids), 0);
	if (n) {
		memcpy(ids, ctx->watchers, n * sizeof(*ids));
	}
	for (i = 0; i < n; i++) {
		struct fpm_worker_watcher *watcher = zend_hash_index_find_ptr(&fw.watchers, ids[i]);

		if (watcher && watcher->ws == ctx) {
			zend_hash_index_del(&fw.watchers, ids[i]);
		}
	}
	efree(ids);
	/* The bufferevent stays evhttp's -- see the comment in
	 * fpmng_worker_upgrade() -- so tearing the connection down is a
	 * shutdown(), not a bufferevent_free(): the client sees the connection
	 * die, and fpm_ws_release() frees the connection (fd, SSL*) once the close
	 * tail below is done (#472). What
	 * is already queued (a close frame, most often -- the codec writes it
	 * and calls fclose() in the same breath) gets its chance first: with
	 * bytes still unwritten, the callbacks are pointed at
	 * fpm_ws_close_after_write(), which half-closes the moment they are on
	 * the wire -- the last thing this connection ever does. */
	if (!ctx->orphaned) {
		int ws_fd = (int) bufferevent_getfd(ctx->bev);
		size_t queued = evbuffer_get_length(bufferevent_get_output(ctx->bev));

		/* An uncaught userland throw destroys its local stream here, before
		 * child_main() can run its generic fatal path. The pending entry was
		 * reaped at the hijack, so this is the last place that can name the
		 * request and the close outcome instead of leaving both silent (#461). */
		if (EG(exception)) {
			fpm_ws_log_failed_upgrade(ctx, ws_fd, queued);
		}
		if (!ctx->close_after_write) {
			if (ws_fd >= 0 && queued > 0) {
				/* fpm_worker_finish_output() must drive the base until this tail
				 * drains. Without the separate counter, evhttp_free() would discard
				 * the queued 101 as soon as an exception stopped worker execution. */
				fpm_ws_start_close_after_write(ctx, true);
			} else {
				fpm_ws_shutdown_now(ctx, true);
			}
		}
		ctx->eof = true;
	}
	efree(ctx->request_uri);
	efree(ctx->watchers);
	efree(ctx);
	return 0;
}

/* The tail of a closed upgraded connection: queued bytes out, then the
 * half-close. arg is the bufferevent -- the context is long gone (freed by
 * fpm_ws_close() above), so this reads nothing but the buffer itself. */
static void fpm_ws_close_after_write(struct bufferevent *bev, short what, void *arg)
{
	size_t queued;

	queued = evbuffer_get_length(bufferevent_get_output(bev));
	if (queued > 0) {
		if (what & BEV_EVENT_EOF) {
			/* EOF is the read side: a client that half-closes its writer can
			 * still receive the queued 101 or close frame. Stop reading, but
			 * leave EV_WRITE armed until the output is actually on the wire. */
			bufferevent_disable(bev, EV_READ);
		}
		if (!(what & BEV_EVENT_ERROR)) {
			return; /* still writing; the write callback fires again */
		}
	}
	/* Both the event and write callback can report the same final transition.
	 * Disarm before shutdown so the count is settled once and a later EOF made
	 * readable by shutdown() cannot deliver this tail through a stale callback. */
	bufferevent_setcb(bev, NULL, NULL, NULL, NULL);
	bufferevent_disable(bev, EV_READ | EV_WRITE);
	fpm_ws_finish_close(bev, true, (what & BEV_EVENT_ERROR) != 0, arg);
}

static void fpm_ws_close_after_write_cb(struct bufferevent *bev, void *arg)
{
	fpm_ws_close_after_write(bev, 0, arg);
}

/* #472: shutdown() ends the conversation but the fd stays open until the
 * bufferevent is freed, and evhttp frees it only at evhttp_free(). A worker
 * with steady WebSocket churn therefore leaked one fd (plus a bufferevent and
 * a request) per closed stream -- measured 1:1 with the number of closed
 * streams. Once the close tail is done the connection is released: not at once,
 * because close() on a socket with unread peer data sends a RST that can
 * discard the close frame or close_notify we just wrote, so the read side is
 * drained until the peer's EOF (immediate after a full shutdown) or a timeout,
 * and only then is the connection freed. The request evhttp left behind is
 * freed with it, exactly as evhttp_free() would have. */
#define FPM_WS_LINGER_SECONDS 5

/* The bound is a hard deadline from the moment of release, not a read idle
 * timeout: libevent restarts an idle timeout on every read, and a peer that
 * keeps sending after our FIN would hold the fd (and the loop, discarding)
 * indefinitely. */
struct fpm_ws_linger {
	struct evhttp_connection *evcon;
	struct event *deadline;
};

static void fpm_ws_linger_end(struct fpm_ws_linger *linger)
{
	/* evhttp_connection_free() clears the bufferevent's callbacks, so nothing
	 * can run on the freed connection afterwards. */
	event_free(linger->deadline);
	evhttp_connection_free(linger->evcon);
	pefree(linger, 1);
}

static void fpm_ws_linger_read(struct bufferevent *bev, void *arg)
{
	(void) arg;
	evbuffer_drain(bufferevent_get_input(bev), evbuffer_get_length(bufferevent_get_input(bev)));
}

static void fpm_ws_linger_event(struct bufferevent *bev, short what, void *arg)
{
	(void) bev;
	(void) what;
	fpm_ws_linger_end(arg); /* EOF or error: the peer is done */
}

static void fpm_ws_linger_deadline(evutil_socket_t fd, short what, void *arg)
{
	(void) fd;
	(void) what;
	fpm_ws_linger_end(arg);
}

static void fpm_ws_release(struct evhttp_connection *evcon)
{
	struct bufferevent *bev = evhttp_connection_get_bufferevent(evcon);
	struct timeval deadline = { FPM_WS_LINGER_SECONDS, 0 };
	struct fpm_ws_linger *linger = pemalloc(sizeof(*linger), 1);

	linger->evcon = evcon;
	linger->deadline = event_new(fw.base, -1, EV_TIMEOUT, fpm_ws_linger_deadline, linger);
	if (!linger->deadline || event_add(linger->deadline, &deadline) != 0) {
		/* No way to bound the linger: leave the connection to evhttp_free(),
		 * as before #472. Callbacks stay disarmed. */
		if (linger->deadline) {
			event_free(linger->deadline);
		}
		pefree(linger, 1);
		return;
	}
	bufferevent_setcb(bev, fpm_ws_linger_read, NULL, fpm_ws_linger_event, linger);
	bufferevent_enable(bev, EV_READ);
}

static void fpm_ws_close_now(struct bufferevent *bev, bool graceful, bool counted,
		struct evhttp_connection *release)
{
	int ws_fd = (int) bufferevent_getfd(bev);

	if (ws_fd >= 0) {
		/* A completed TLS shutdown keeps the read half open: closing it while
		 * unread peer data is queued can turn the socket's FIN/RST choice into
		 * a reset and discard the close_notify we just accepted. Plaintext keeps
		 * the historical full shutdown. */
		shutdown(ws_fd, graceful ? SHUT_WR : SHUT_RDWR);
	}
	if (counted && fw.ws_unflushed) {
		fw.ws_unflushed--;
	}
	if (release) {
		fpm_ws_release(release);
	}
}

static void fpm_ws_tls_close_unlink(struct fpm_ws_tls_close *state)
{
	struct fpm_ws_tls_close **p = &fpm_ws_tls_closing;

	while (*p && *p != state) {
		p = &(*p)->next;
	}
	if (*p) {
		*p = state->next;
	}
}

static void fpm_ws_tls_close_complete(struct fpm_ws_tls_close *state, bool graceful)
{
	fpm_ws_tls_close_unlink(state);
	/* The dependent retry event goes before shutdown(), which can make the fd
	 * readable immediately. evhttp still owns both the bufferevent and SSL*. */
	event_free(state->event);
	fpm_ws_close_now(state->bev, graceful, true, state->release);
	pefree(state, 1);
}

static void fpm_ws_tls_close_retry(evutil_socket_t fd, short events, void *arg)
{
	struct fpm_ws_tls_close *state = arg;
	short poll_events = 0;
	int result;

	(void) events;
	if (fd < 0 || (result = fpm_http_direct_tls_shutdown_step(state->bev, &poll_events)) == FPM_HTTP_DIRECT_TLS_SHUTDOWN_PENDING) {
		if (fd >= 0 && poll_events && event_del(state->event) == 0 && event_assign(state->event, fw.base, fd, poll_events, fpm_ws_tls_close_retry, state) == 0 && event_add(state->event, NULL) == 0) {
			return;
		}
		fpm_ws_tls_close_complete(state, false);
		return;
	}
	fpm_ws_tls_close_complete(state,
			result == FPM_HTTP_DIRECT_TLS_SHUTDOWN_SENT);
}

static void fpm_ws_tls_close_arm(struct bufferevent *bev, short poll_events, bool already_counted,
		struct evhttp_connection *release)
{
	struct fpm_ws_tls_close *state;
	int fd = (int) bufferevent_getfd(bev);

	if (fd < 0 || poll_events == 0) {
		fpm_ws_close_now(bev, false, already_counted, release);
		return;
	}
	state = pemalloc(sizeof(*state), 1);
	if (!state) {
		fpm_ws_close_now(bev, false, already_counted, release);
		return;
	}
	state->bev = bev;
	state->release = release;
	state->next = NULL;
	state->event = event_new(fw.base, fd, poll_events, fpm_ws_tls_close_retry, state);
	if (!state->event) {
		pefree(state, 1);
		fpm_ws_close_now(bev, false, already_counted, release);
		return;
	}
	if (!already_counted) {
		fw.ws_unflushed++;
	}
	/* The child is single-threaded, so event_add() cannot dispatch the callback
	 * before the state is linked; link before returning to the owning loop. */
	if (event_add(state->event, NULL) != 0) {
		fpm_ws_close_now(bev, false, true, release);
		event_free(state->event);
		pefree(state, 1);
		return;
	}
	state->next = fpm_ws_tls_closing;
	fpm_ws_tls_closing = state;
}

static void fpm_ws_finish_close(struct bufferevent *bev, bool counted, bool transport_error,
		struct evhttp_connection *release)
{
	short poll_events = 0;
	int result;

	if (transport_error) {
		/* The transport already reported a fatal read/write condition. Calling
		 * SSL_shutdown() on it cannot produce a clean close_notify and risks
		 * consuming unrelated OpenSSL error state. */
		fpm_ws_close_now(bev, false, counted, release);
		return;
	}
	result = fpm_http_direct_tls_shutdown_step(bev, &poll_events);
	switch (result) {
		case FPM_HTTP_DIRECT_TLS_SHUTDOWN_NOT_APPLICABLE:
			fpm_ws_close_now(bev, false, counted, release);
			break;
		case FPM_HTTP_DIRECT_TLS_SHUTDOWN_PENDING:
			fpm_ws_tls_close_arm(bev, poll_events, counted, release);
			break;
		case FPM_HTTP_DIRECT_TLS_SHUTDOWN_SENT:
			fpm_ws_close_now(bev, true, counted, release);
			break;
		default:
			fpm_ws_close_now(bev, false, counted, release);
			break;
	}
}

void fpm_ws_tls_close_abort_all(void)
{
	while (fpm_ws_tls_closing) {
		fpm_ws_tls_close_complete(fpm_ws_tls_closing, false);
	}
}

/* A retained upgraded stream does not run fpm_ws_close() when userland
 * unwinds. On an abnormal worker exit, visit those contexts before evhttp_free()
 * so the same bounded close-after-write path covers both local and retained
 * streams (#461). */
void fpm_ws_prepare_shutdown(bool log_failure)
{
	struct fpm_ws_ctx *ctx;

	for (ctx = fpm_ws_all; ctx; ctx = ctx->next) {
		int ws_fd;
		size_t queued;

		if (ctx->orphaned) {
			continue;
		}
		ws_fd = (int) bufferevent_getfd(ctx->bev);
		queued = evbuffer_get_length(bufferevent_get_output(ctx->bev));
		if (log_failure) {
			fpm_ws_log_failed_upgrade(ctx, ws_fd, queued);
		}
		if (ctx->close_after_write) {
			continue;
		}
		if (ws_fd >= 0 && queued > 0) {
			fpm_ws_start_close_after_write(ctx, false);
		} else {
			fpm_ws_shutdown_now(ctx, false);
		}
	}
}

static int fpm_ws_cast(php_stream *stream, int castas, void **ret)
{
	struct fpm_ws_ctx *ctx = stream->abstract;
	int fd;

	if (ctx->orphaned) {
		/* The fd belongs to the freed bufferevent (#443): nothing to cast. */
		return FAILURE;
	}
	fd = (int) bufferevent_getfd(ctx->bev);

	switch (castas) {
		case PHP_STREAM_AS_FD:
		case PHP_STREAM_AS_FD_FOR_SELECT:
		case PHP_STREAM_AS_SOCKETD:
			if (fd < 0) {
				return FAILURE;
			}
			if (ret) {
				*(int *) ret = fd;
			}
			return SUCCESS;
		default:
			return FAILURE;
	}
}

static int fpm_ws_set_option(php_stream *stream, int option, int value, void *ptrparam)
{
	struct fpm_ws_ctx *ctx = stream->abstract;

	(void) value;
	(void) ptrparam;
	switch (option) {
		case PHP_STREAM_OPTION_CHECK_LIVENESS:
			/* _php_stream_eof() marks the stream EOF when this reports the
			 * stream unlivable -- the answer must come from the bufferevent's
			 * event callback (fpm_ws_eventcb()), never from "the buffer is
			 * empty right now", which on a WebSocket is the NORMAL state. */
			return ctx->eof ? PHP_STREAM_OPTION_RETURN_ERR : PHP_STREAM_OPTION_RETURN_OK;
		default:
			return PHP_STREAM_OPTION_RETURN_NOTIMPL;
	}
}

static const php_stream_ops fpm_ws_ops = {
	fpm_ws_write, /* write */
	fpm_ws_read, /* read */
	fpm_ws_close, /* close */
	NULL, /* flush: bytes reach the socket when the loop drains the bufferevent */
	"fpmng-ws", /* label */
	NULL, /* seek: a stream is not seekable */
	fpm_ws_cast, /* cast */
	NULL, /* stat */
	fpm_ws_set_option, /* set_option */
};

bool fpm_ws_is_ws_stream(php_stream *stream)
{
	return stream->ops == &fpm_ws_ops;
}

static bool fpm_ws_header_has_token(struct evkeyvalq *headers, const char *name, const char *token)
{
	struct evkeyval *header;
	size_t token_len = strlen(token);

	TAILQ_FOREACH(header, headers, next)
	{
		const char *p, *start, *end;

		if (strcasecmp(header->key, name) != 0) {
			continue;
		}
		p = header->value;
		while (*p) {
			while (*p == ',' || *p == ' ' || *p == '\t') {
				p++;
			}
			start = p;
			while (*p && *p != ',') {
				p++;
			}
			end = p;
			while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
				end--;
			}
			if ((size_t) (end - start) == token_len && strncasecmp(start, token, token_len) == 0) {
				return true;
			}
		}
	}
	return false;
}

static int fpm_ws_base64_value(unsigned char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

/* RFC 6455 section 4.2.1 requires canonical base64 which decodes to exactly 16
 * bytes. The worker child has no Zend MM, so validate into a fixed stack buffer. */
static bool fpm_ws_key_valid(const char *key)
{
	unsigned char decoded[16];
	int values[4];
	size_t i, out = 0;

	if (strlen(key) != 24 || key[22] != '=' || key[23] != '=') {
		return false;
	}
	for (i = 0; i < 20; i++) {
		values[i % 4] = fpm_ws_base64_value((unsigned char) key[i]);
		if (values[i % 4] < 0) {
			return false;
		}
		if (i % 4 == 3) {
			decoded[out++] = (unsigned char) ((values[0] << 2) | (values[1] >> 4));
			decoded[out++] = (unsigned char) ((values[1] << 4) | (values[2] >> 2));
			decoded[out++] = (unsigned char) ((values[2] << 6) | values[3]);
		}
	}
	/* The final pair of base64 sextets encodes one byte; its low four padding
	 * bits must be zero for the representation to be canonical. */
	values[0] = fpm_ws_base64_value((unsigned char) key[20]);
	values[1] = fpm_ws_base64_value((unsigned char) key[21]);
	if (values[0] < 0 || values[1] < 0 || (values[1] & 0x0f) != 0) {
		return false;
	}
	decoded[out++] = (unsigned char) ((values[0] << 2) | (values[1] >> 4));
	return out == sizeof(decoded);
}

static unsigned fpm_ws_header_count(struct evkeyvalq *headers, const char *name, const char **value)
{
	struct evkeyval *header;
	unsigned count = 0;

	*value = NULL;
	TAILQ_FOREACH(header, headers, next)
	{
		if (strcasecmp(header->key, name) == 0) {
			count++;
			*value = header->value;
		}
	}
	return count;
}

/* Answer a malformed/unsupported upgrade before changing connection ownership.
 * These are ordinary completed requests: keep scoreboard and pm.max_requests
 * accounting identical to fpmng_worker_respond(). */
static void fpm_ws_reject_upgrade(struct fpm_worker_pending *p, int status, bool version_required)
{
	struct evhttp_request *http = p->http;
	struct evhttp_connection *connection = evhttp_request_get_connection(http);
	struct evbuffer *body;

	if (!connection) {
		fpm_worker_reap(p);
		return;
	}
	if (version_required && evhttp_add_header(evhttp_request_get_output_headers(http),
									"Sec-WebSocket-Version", "13") != 0) {
		status = 500;
	}
	if (fpm_worker_stopping || (fw.wp->config->pm_max_requests &&
									   fw.answered + 1 >= (unsigned) fw.wp->config->pm_max_requests)) {
		evhttp_add_header(evhttp_request_get_output_headers(http), "Connection", "close");
	}
	evhttp_connection_set_closecb(connection, NULL, NULL);
	body = evbuffer_new();
	if (body) {
		fpm_worker_send_reply(http, status, body);
		evbuffer_free(body);
	} else {
		fpm_worker_send_error(http, 500, NULL);
	}
	fpm_worker_reap(p);
	fpm_worker_account_answered_request();
}

/* Sec-WebSocket-Accept: base64(SHA1(key || GUID)), RFC 6455 section 4.2.1.
 * ext/standard's SHA1 and base64 are PHPAPI and always linked. */
static zend_string *fpm_ws_accept_key(const char *ws_key)
{
	static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	PHP_SHA1_CTX sha;
	unsigned char digest[20];
	size_t key_len = strlen(ws_key);
	char *buf = emalloc(key_len + sizeof(guid));

	/* sized to the actual key: a truncated key||GUID hashes to the wrong
	 * Accept and the compliant client hangs on the handshake */
	memcpy(buf, ws_key, key_len);
	memcpy(buf + key_len, guid, sizeof(guid));
	PHP_SHA1Init(&sha);
	PHP_SHA1Update(&sha, (const unsigned char *) buf, key_len + sizeof(guid) - 1);
	PHP_SHA1Final(digest, &sha);
	efree(buf);
	return php_base64_encode(digest, sizeof(digest));
}

/* Returns an ordinary bidirectional php_stream over the hijacked connection:
 * fread()/fwrite()/fclose()/feof()/stream_has_buffered() work on it, and so do
 * the watcher primitives, which see the underlying fd. Throws before state
 * changes when the id is unknown/answered or the client is gone. A malformed
 * WebSocket handshake is answered here (400, or RFC-required 426 for an
 * unsupported/missing version) and returns NULL. */
ZEND_FUNCTION(fpmng_worker_upgrade)
{
	zend_long id;
	HashTable *response_headers;
	struct fpm_worker_pending *p;
	struct fpm_ws_ctx *ctx;
	struct evhttp_connection *connection;
	struct bufferevent *bev;
	struct evkeyvalq *in;
	const char *upgrade, *key, *version;
	zend_string *accept;
	unsigned upgrade_count, key_count, version_count;
	smart_str head = { 0 };
	zend_string *name;
	zval *value;
	php_stream *stream;

	ZEND_PARSE_PARAMETERS_START(2, 2)
	Z_PARAM_LONG(id)
	Z_PARAM_ARRAY_HT(response_headers)
	ZEND_PARSE_PARAMETERS_END();

	p = fpm_worker_pending_get(id);
	if (!p || !p->http) {
		zend_argument_value_error(1, "is not an in-flight request");
		RETURN_THROWS();
	}
	in = evhttp_request_get_input_headers(p->http);
	upgrade_count = fpm_ws_header_count(in, "Upgrade", &upgrade);
	key_count = fpm_ws_header_count(in, "Sec-WebSocket-Key", &key);
	version_count = fpm_ws_header_count(in, "Sec-WebSocket-Version", &version);
	if (evhttp_request_get_command(p->http) != EVHTTP_REQ_GET || upgrade_count != 1 || !upgrade || evutil_ascii_strcasecmp(upgrade, "websocket") != 0) {
		zend_argument_value_error(1, "is not a WebSocket upgrade request");
		RETURN_THROWS();
	}
	if (!fpm_ws_header_has_token(in, "Connection", "Upgrade") || key_count != 1 || !key || !fpm_ws_key_valid(key)) {
		fpm_ws_reject_upgrade(p, 400, false);
		RETURN_NULL();
	}
	if (version_count != 1 || !version || strcmp(version, "13") != 0) {
		fpm_ws_reject_upgrade(p, 426, true);
		RETURN_NULL();
	}

	accept = fpm_ws_accept_key(key);

	smart_str_appends(&head, "HTTP/1.1 101 Switching Protocols\r\n");
	smart_str_appends(&head, "Upgrade: websocket\r\n");
	smart_str_appends(&head, "Connection: Upgrade\r\n");
	smart_str_appends(&head, "Sec-WebSocket-Accept: ");
	smart_str_appends(&head, ZSTR_VAL(accept));
	smart_str_appends(&head, "\r\n");
	zend_string_release(accept);

	/* Caller headers (Sec-WebSocket-Protocol and friends), validated the same
	 * way every other response header this executor sends is: the name must
	 * be an RFC 9110 token, the hop-by-hop list still applies (Upgrade and
	 * Connection are this function's to set), and one bad header refuses the
	 * upgrade -- nothing has been written yet, so the request remains
	 * answerable. */
	ZEND_HASH_FOREACH_STR_KEY_VAL(response_headers, name, value)
	{
		if (!name || !fpm_http_direct_header_name_ok(ZSTR_VAL(name)) || fpm_http_direct_header_dropped(ZSTR_VAL(name))) {
			smart_str_free(&head);
			zend_argument_value_error(2, "contains an invalid or hop-by-hop header name");
			RETURN_THROWS();
		}
		if (Z_TYPE_P(value) != IS_STRING) {
			smart_str_free(&head);
			zend_argument_value_error(2, "must be an array of strings");
			RETURN_THROWS();
		}
		{
			/* The value goes into a hand-written response head, bypassing
			 * evhttp_add_header()'s own validation -- a \r\n in it would be
			 * response splitting, and the value is often the client's own
			 * Sec-WebSocket-Protocol echoed back. */
			const char *v = Z_STRVAL_P(value);
			size_t vlen = Z_STRLEN_P(value), vi;

			for (vi = 0; vi < vlen; vi++) {
				if (v[vi] == '\r' || v[vi] == '\n' || v[vi] == '\0') {
					smart_str_free(&head);
					zend_argument_value_error(2, "contains a header value with a control character");
					RETURN_THROWS();
				}
			}
		}
		smart_str_appends(&head, ZSTR_VAL(name));
		smart_str_appends(&head, ": ");
		smart_str_appends(&head, Z_STR_P(value) ? Z_STRVAL_P(value) : "");
		smart_str_appends(&head, "\r\n");
	}
	ZEND_HASH_FOREACH_END();
	smart_str_appends(&head, "\r\n");

	ctx = emalloc(sizeof(*ctx));
	memset(ctx, 0, sizeof(*ctx));

	/* evhttp owns the connection until the request is answered -- which it
	 * never will be. The hijack follows evws_new_session()'s
	 * evhttp_start_ws_() step for step, on the public 2.1 API -- and where
	 * 2.2's internal version steals evcon->bufev and frees the connection,
	 * this one keeps BOTH alive: the public API can neither reach
	 * evcon->bufev (to null it before a free) nor detach a connection from
	 * its evhttp, and 2.1's own evhttp_connection_free() shutdown(fd,
	 * SHUT_WR)s the connection on the way out, which would kill the
	 * WebSocket the moment it was born. So:
	 *
	 *   - our callbacks replace evhttp's, and its state machine on this
	 *     connection is never driven again;
	 *   - the closecb and the timeouts go (an idle WebSocket must not be cut
	 *     by http.read_timeout; liveness is the codec's ping/pong);
	 *   - the request is left exactly where evhttp put it: never answered,
	 *     never driven, freed with the connection at evhttp_free();
	 *   - the stream's close does NOT free the bufferevent -- it drains
	 *     output, performs TLS shutdown when applicable, and closes the fd,
	 *     because evhttp still owns the bufferevent and frees it (fd and SSL)
	 *     at teardown; a registry of hijacked connections
	 *     (fpm_ws_all, walked in child_main() just before evhttp_free())
	 *     tells a late stream free the bufferevent is gone.
	 *
	 * A closed stream releases the connection through fpm_ws_release()
	 * (#472); only a connection still open at teardown is left to
	 * evhttp_free(). */
	connection = evhttp_request_get_connection(p->http);
	bev = connection ? evhttp_connection_get_bufferevent(connection) : NULL;
	if (!bev) {
		/* the client is already gone */
		smart_str_free(&head);
		efree(ctx);
		fpm_worker_reap(p);
		RETURN_FALSE;
	}
	ctx->bev = bev;
	ctx->evcon = connection;
	ctx->request_id = p->id;
	ctx->request_uri = estrdup(evhttp_request_get_uri(p->http));

	/* Everything that can fail now happens BEFORE any of evhttp's callbacks
	 * are replaced: a failure path below leaves the connection exactly as the
	 * accept built it -- the request is still answerable by
	 * fpmng_worker_respond(), the same contract every throw above keeps. */
	stream = php_stream_alloc(&fpm_ws_ops, ctx, 0, "r+b");
	if (!stream) {
		smart_str_free(&head);
		efree(ctx->request_uri);
		efree(ctx);
		RETURN_FALSE;
	}
	ctx->stream = stream;
	ctx->next = fpm_ws_all;
	fpm_ws_all = ctx;

	/* The point of no return, one run with no loop iteration inside: evhttp's
	 * callbacks go, ours go in, the 101 is written. */
	bufferevent_setcb(bev, fpm_ws_readcb, fpm_ws_writecb, fpm_ws_eventcb, ctx);
	bufferevent_set_timeouts(bev, NULL, NULL);
	bufferevent_enable(bev, EV_READ | EV_WRITE);
	evhttp_connection_set_closecb(connection, NULL, NULL);
	p->http = NULL;

	if (bufferevent_write(bev, ZSTR_VAL(head.s), ZSTR_LEN(head.s)) != 0) {
		/* The callback swap above is the point of no return. An evbuffer
		 * allocation failure at this exact write is not black-box reachable,
		 * but returning a live-looking stream without a queued 101 would
		 * silently strand the client. This guard is deliberate insurance, as
		 * recorded in #440 item 7. */
		php_stream_close(stream);
		smart_str_free(&head);
		fpm_worker_reap(p);
		fpm_worker_account_answered_request();
		RETURN_FALSE;
	}
	smart_str_free(&head);

	/* The connection leaves the pending world: worker.max_pending and
	 * worker.request_timeout do not apply to it any more, and it counts
	 * towards pm.max_requests like any other answered request -- the recycle
	 * tail is fpmng_worker_respond_end()'s, without the reply counting (the
	 * 101 never goes through fpm_worker_count_reply()). */
	fpm_worker_reap(p);
	fpm_worker_account_answered_request();

	RETURN_RES(stream->res);
}
