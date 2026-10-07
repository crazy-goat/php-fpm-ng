/* Flow control from a slow gateway client back to the upstream (issue #596),
 * and the minimum rate that client must sustain while the upstream is held
 * (issue #705).
 *
 * Before #596, the gateway appended every piece of the upstream response to the
 * client's evhttp output buffer as fast as the upstream produced it
 * (evhttp_send_reply_chunk() has no limit and gives no feedback), so a client
 * that stopped reading kept the whole response in the memory of the one process
 * that serves every connection of the pool. The write timeout of issue #593
 * bounds how long that lasts, not how much memory it takes.
 *
 * The #596 rule: after each piece, if more than http.response_buffer bytes of
 * this client's output are still unwritten, the read event of the upstream that
 * serves this request is removed. The kernel then fills the socket buffers
 * towards the worker and the worker blocks in its own write, which is the
 * backpressure. The event is added back when evhttp reports the client's
 * output buffer empty, or when the request is over or the client is gone.
 *
 * Resuming at "empty" and not at a lower watermark is deliberate: the hook
 * evhttp offers is the per-write completion callback of
 * evhttp_send_reply_chunk_with_cb(), which libevent runs from its write
 * callback (watermark 0). Replacing that write callback to get a lower mark
 * would break evhttp's own state machine. The cost is a short idle gap on the
 * upstream each time the buffer drains, only for clients slow enough to have
 * reached the limit.
 *
 * The #705 rule, the follow-up #596 left open: holding the worker back gives a
 * trickling client a way to hold it indefinitely. http.write_timeout is a stall
 * timer -- it restarts on every byte the client takes -- so a client that reads
 * one byte per second is never cut by it and keeps a PHP/target worker blocked
 * for as long as it trickles. While the upstream is paused (the only time the
 * client is the bottleneck, so a slow UPSTREAM is never mistaken for a slow
 * client), a periodic check requires the client to have drained at least
 * http.response_min_rate bytes per second over the last window. Below that the
 * connection is cut the same safe way the read deadline cuts one, and the
 * worker's remaining output is drained as if the client had left. The minimum
 * is a rate and not a total-time cap on purpose: a legitimately slow but
 * progressing download is not cut, only one that has effectively stalled while
 * still consuming a byte now and then.
 *
 * Only the read of ONE upstream stops. With pinned upstreams one upstream
 * serves one request (fpm_http_upstream_s.current), so no other request is
 * delayed. */

#include "fpm_config.h"
#include "fpm_http.h"

#ifdef HAVE_FPM_HTTP

#include "fpm_http_internal.h"
#include "zlog.h"

/* evhttp has written everything of this client's output: the upstream may be
 * read again. `arg` is the connection's node, which outlives every write
 * callback of its evhttp connection (it is freed by the close callback). */
static void fpm_http_response_drained(struct evhttp_connection *evcon, void *arg)
{
	struct fpm_http_client_s *cl = arg;

	(void) evcon;
	if (cl->c) {
		fpm_http_response_resume(cl->c);
	}
}

/* The window over which http.response_min_rate is measured. Fixed, see
 * FPM_HTTP_RESPONSE_MIN_RATE_WINDOW_MS. */
static const struct timeval fpm_http_minrate_window = {
	FPM_HTTP_RESPONSE_MIN_RATE_WINDOW_MS / 1000,
	(FPM_HTTP_RESPONSE_MIN_RATE_WINDOW_MS % 1000) * 1000
};

/* The client has not drained http.response_min_rate bytes/s over the last
 * window while the upstream was held back for it: end the connection.
 *
 * The connection is ended by shrinking its own timeouts so evhttp closes it
 * through its error path -- the same, and only safe, way
 * fpm_http_client_idle_fire() ends one (a bufferevent_free() underneath evhttp
 * is a use-after-free, issue #90). There is pending output by construction, so
 * the one-microsecond write timeout fires on the next loop pass. No re-arm:
 * the event is one-shot and fpm_http_conn_free() frees it as the connection
 * closes. */
static void fpm_http_response_minrate_fire(evutil_socket_t fd, short what, void *arg)
{
	fpm_http_conn *c = arg;
	struct fpm_http_client_s *cl = c->client;
	struct bufferevent *bev;
	size_t outlen, drained;
	long long required;

	(void) fd;
	(void) what;
	/* The timer is stopped by fpm_http_response_resume() and freed with the
	 * request, so reaching here with the pause cleared should not happen; if
	 * it does, there is nothing to enforce. */
	if (!c->read_paused || !cl || !cl->evcon) {
		return;
	}
	bev = evhttp_connection_get_bufferevent(cl->evcon);
	if (!bev) {
		return;
	}
	outlen = evbuffer_get_length(bufferevent_get_output(bev));
	/* While the upstream is paused nothing new is queued, so the client's
	 * unwritten bytes only shrink; the difference is what it drained. */
	drained = c->minrate_last_outlen > outlen ? c->minrate_last_outlen - outlen : 0;
	required = (long long) c->gw->response_min_rate * FPM_HTTP_RESPONSE_MIN_RATE_WINDOW_MS / 1000;
	if ((long long) drained < required) {
		static const struct timeval now = { 0, 1 };

		zlog(ZLOG_WARNING, "[pool %s] http: client drained %llu bytes in %d ms while a worker was held for it, "
						   "below http.response_min_rate = %d bytes/s; closing the connection",
				c->gw->pool, (unsigned long long) drained, FPM_HTTP_RESPONSE_MIN_RATE_WINDOW_MS,
				c->gw->response_min_rate);
		bufferevent_set_timeouts(bev, &now, &now);
		return;
	}
	c->minrate_last_outlen = outlen;
	event_add(c->minrate_timer, &fpm_http_minrate_window);
}

void fpm_http_response_resume(fpm_http_conn *c)
{
	fpm_http_upstream *up = c->upstream;

	/* Issue #705: the client is progressing (or the request is over), so the
	 * minimum-rate clock stops. Done before the read_paused early return
	 * because fpm_http_finish() calls this on every request, not only a
	 * paused one. */
	if (c->minrate_timer) {
		event_del(c->minrate_timer);
	}
	if (!c->read_paused) {
		return;
	}
	c->read_paused = 0;
	/* No timeout: the upstream is busy, and the idle timer is re-armed by
	 * fpm_http_request_done() when this request ends. */
	if (up && !up->dead && up->fd >= 0) {
		event_add(up->ev_read, NULL);
		/* Issue #716: the upstream read deadline starts running again, with a
		 * full interval, because the pause stopped it -- the gateway chose not to
		 * read, so no progress could be observed and the pause must not be charged
		 * to the target. arm() re-reads up->current->read_paused, which is what
		 * makes this single call the right one for the other two callers of
		 * fpm_http_response_resume() (the client close callback and
		 * fpm_http_finish()) too. */
		fpm_http_upstream_deadline_arm(up);
	}
}

void fpm_http_response_chunk(fpm_http_conn *c, const char *data, size_t len)
{
	struct evbuffer *chunk = evbuffer_new();
	struct fpm_http_client_s *cl = c->client;

	evbuffer_add(chunk, data, len);
	/* The callback also fires on every drain while nothing is paused; it then
	 * finds read_paused clear and does nothing. Passed on every piece, not
	 * only on the one that crosses the limit, because the next plain
	 * evhttp_send_reply_chunk() would overwrite it with NULL. */
	if (cl) {
		evhttp_send_reply_chunk_with_cb(c->req, chunk, fpm_http_response_drained, cl);
	} else {
		evhttp_send_reply_chunk(c->req, chunk);
	}
	evbuffer_free(chunk);
	c->bytes_out += len;

	if (cl && c->gw->response_buffer > 0 && !c->read_paused && c->upstream && c->evcon) {
		struct bufferevent *bev = evhttp_connection_get_bufferevent(c->evcon);

		if (bev && evbuffer_get_length(bufferevent_get_output(bev)) > c->gw->response_buffer) {
			c->read_paused = 1;
			event_del(c->upstream->ev_read);
			/* Issue #705: start the minimum-rate clock. The timer is
			 * created lazily; on OOM the connection is still bounded by
			 * http.write_timeout, the gateway still works. */
			if (c->gw->response_min_rate > 0) {
				if (!c->minrate_timer) {
					c->minrate_timer = event_new(c->gw->base, -1, EV_TIMEOUT,
							fpm_http_response_minrate_fire, c);
				}
				if (c->minrate_timer) {
					c->minrate_last_outlen = evbuffer_get_length(bufferevent_get_output(bev));
					event_add(c->minrate_timer, &fpm_http_minrate_window);
				}
			}
			/* Issue #716: the upstream read deadline stops with the read. The
			 * worker behind it is blocked in its write for as long as this pause
			 * lasts, which is not its silence, so charging http.upstream_read_timeout
			 * for it would time out a target that is producing perfectly well into
			 * a socket buffer nobody is draining. */
			fpm_http_upstream_deadline_stop(c->upstream);
		}
	}
}

#endif /* HAVE_FPM_HTTP */
