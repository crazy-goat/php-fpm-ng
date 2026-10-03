/* Flow control from a slow gateway client back to the upstream (issue #596).
 *
 * Before this, the gateway appended every piece of the upstream response to the
 * client's evhttp output buffer as fast as the upstream produced it
 * (evhttp_send_reply_chunk() has no limit and gives no feedback), so a client
 * that stopped reading kept the whole response in the memory of the one process
 * that serves every connection of the pool. The write timeout of issue #593
 * bounds how long that lasts, not how much memory it takes.
 *
 * The rule: after each piece, if more than http.response_buffer bytes of this
 * client's output are still unwritten, the read event of the upstream that
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
 * Only the read of ONE upstream stops. With pinned upstreams one upstream
 * serves one request (fpm_http_upstream_s.current), so no other request is
 * delayed. */

#include "fpm_config.h"
#include "fpm_http.h"

#ifdef HAVE_FPM_HTTP

#include "fpm_http_internal.h"

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

void fpm_http_response_resume(fpm_http_conn *c)
{
	fpm_http_upstream *up = c->upstream;

	if (!c->read_paused) {
		return;
	}
	c->read_paused = 0;
	/* No timeout: the upstream is busy, and the idle timer is re-armed by
	 * fpm_http_request_done() when this request ends. */
	if (up && !up->dead && up->fd >= 0) {
		event_add(up->ev_read, NULL);
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
		}
	}
}

#endif /* HAVE_FPM_HTTP */
