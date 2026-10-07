/* Upstream-side deadlines for the gateway's own connections (issue #716).
 *
 * Until now the gateway bounded its CLIENT side and nothing on the other side:
 * the non-blocking connect towards a target waited for EV_WRITE with no
 * timeout (fpm_http_transport_connect()), and once the request was on the wire
 * the target's read event was re-added with no timeout (fpm_http_pump_target()).
 * A target that never answered -- a black-holed address, an accept queue the
 * kernel has stopped draining, a worker or an http-direct target that took the
 * request and then stopped producing -- held the client connection and one slot
 * of the target's admission budget for as long as it liked. A FastCGI target
 * with request_terminate_timeout set ended such a request eventually; with the
 * default 0 (unlimited) nothing did, and a fiber target refuses the directive
 * altogether (#610), so there was nothing to raise either.
 *
 * ONE timer per upstream connection covers both waits, which is what keeps it
 * small enough to audit:
 *
 *   - while up->connecting is set, it is http.upstream_connect_timeout;
 *   - while a request is in flight, it is http.upstream_read_timeout, and it
 *     measures time without progress rather than total time -- both transports'
 *     read callbacks re-arm it on every read that produced bytes.
 *
 * A separate timer rather than a timeout on one of the connection's two existing
 * events, for reasons that are all load-bearing: ev_read already carries a
 * second, unrelated deadline (http.idle_timeout while the connection is idle),
 * ev_write is pending both for a connect and for a socket whose buffer is full,
 * and neither event's timeout would be this code's to re-arm after every byte of
 * progress (for ev_read that is libevent's own business, for ev_write it would
 * mean bounding the flush as well as the connect).
 *
 * The client is answered only while nothing of its response is on the wire: a
 * 504 is truthful then and leaves the keep-alive connection usable. Once the
 * response head is out the reply can no longer be replaced, and the only
 * truthful ending is the one issue #533 established for a lost reply --
 * fpm_http_finish_truncated(), which shuts the client socket down instead of
 * terminating the response.
 *
 * Lifecycle (the gateway's SIGSEGV history: issues #90 and #443). The timer is
 * created once per connection, stopped on every path that ends or pauses it
 * (fpm_http_upstream_detach(), the two backpressure sites) and freed with the
 * struct in fpm_http_upstream_free(). Freeing it from inside its own callback
 * is the case libevent documents as safe -- a non-persistent event is already
 * non-pending by the time its callback runs -- the same one
 * fpm_http_wait_expired() relies on for c->wait_timer (issue #309); measured on
 * the build container against libevent 2.1.12-stable with an ASan build of a
 * timer that event_free()s itself: one callback, no report.
 */

#include "fpm_config.h"

#include "fpm_http.h"

#ifdef HAVE_FPM_HTTP

#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <event2/event.h>
#include <event2/http.h>

#include "fpm_http_internal.h"
#include "zlog.h"

/* Answers one request whose target went silent: 504 Gateway Timeout, and the
 * client connection survives. Reached only while nothing of the response is on
 * the wire (see fpm_http_upstream_deadline_expired() for the other two shapes),
 * so no header has been sent and no body has been promised. */
static void fpm_http_upstream_timeout_reply(fpm_http_conn *c)
{
	c->status = FPM_HTTP_GATEWAY_TIMEOUT;
	evhttp_send_error(c->req, FPM_HTTP_GATEWAY_TIMEOUT, "Gateway Timeout");
	fpm_http_log_response(c->gw, c->req,
			c->remote_addr[0] ? c->remote_addr : c->peer_addr,
			c->remote_user, c->status, c->bytes_out,
			c->log_target ? c->log_target : c->target->pool);
	fpm_http_conn_free(c);
}

static void fpm_http_upstream_deadline_fire(evutil_socket_t fd, short what, void *arg)
{
	(void) fd;
	(void) what;
	fpm_http_upstream_deadline_expired(arg);
}

/* The deadline ran out. Which of the two it was is decided by up->connecting
 * alone, so the log line and the answer cannot disagree: while a connect is
 * outstanding nothing of the request has reached the target, so the connect is
 * what the request is waiting for.
 *
 * Only the timer above calls this. It is named and non-static because the tests
 * and a future caller need one place that ends a connection on time, and
 * keeping it out of the static callback is what makes that place findable. */
void fpm_http_upstream_deadline_expired(fpm_http_upstream *up)
{
	struct fpm_http_gateway_s *gw = up->gw;

	/* Stopped before anything below runs, because the two of them can arm the
	 * timer again on the way out: fpm_http_finish() calls
	 * fpm_http_response_resume() for a paused upstream. The connection ends in
	 * every case, so the deadline is spent either way. */
	fpm_http_upstream_deadline_stop(up);

	if (up->connecting) {
		zlog(ZLOG_WARNING, "[pool %s] http: connect to upstream '%s' did not complete within "
						   "http.upstream_connect_timeout (%d ms); the request is answered 504",
				gw->pool, up->t->listen_address, gw->upstream_connect_timeout_ms);
	} else {
		zlog(ZLOG_WARNING, "[pool %s] http: upstream '%s' made no progress within "
						   "http.upstream_read_timeout (%d ms)",
				gw->pool, up->t->listen_address, gw->upstream_read_timeout_ms);
	}

	/* The same three shapes as fpm_http_upstream_fail(), for the same reasons: a
	 * lost reply after the head was sent is cut rather than terminated (#533), a
	 * reply already under way (issue #594's discard_upstream, whose 502 is a
	 * chunked reply of its own) is simply ended, and only a request that has put
	 * nothing on the wire gets a status of its own. */
	if (up->current) {
		fpm_http_conn *c = up->current;

		if (c->headers_sent && !c->discard_upstream) {
			zlog(ZLOG_WARNING, "[pool %s] http: upstream '%s' timed out after the response head was "
							   "sent; the client connection is closed without completing the reply",
					gw->pool, up->t->listen_address);
			fpm_http_finish_truncated(c);
		} else if (c->headers_sent) {
			fpm_http_finish(c, 1); /* the line above is already in the log */
		} else {
			fpm_http_upstream_timeout_reply(c);
		}
		up->current = NULL;
	}
	/* The connection itself is useless either way: a connect that never
	 * completed has no peer to talk to, and a target that stopped mid-request
	 * may resume in the middle of a reply the gateway has already abandoned.
	 * Dropping it is also what returns its budget slot, and the pump then hands
	 * the target's workers to the next queued request -- the same tail
	 * fpm_http_upstream_fail() has. */
	up->t->ops->drop(up);
	fpm_http_pump(gw);
}

void fpm_http_upstream_deadline_new(fpm_http_upstream *up)
{
	if (!up->deadline) {
		/* A one-shot timer on no fd, like the gateway's other deadlines
		 * (fpm_http_read_deadline_s, the keep-alive timer). EV_PERSIST is
		 * deliberately absent: it is re-armed on purpose, one full interval per
		 * byte of progress. An allocation failure leaves the connection
		 * unbounded, the way an untracked client connection is. */
		up->deadline = event_new(up->gw->base, -1, EV_TIMEOUT,
				fpm_http_upstream_deadline_fire, up);
	}
}

void fpm_http_upstream_deadline_stop(fpm_http_upstream *up)
{
	if (up->deadline) {
		event_del(up->deadline);
	}
}

void fpm_http_upstream_deadline_arm(fpm_http_upstream *up)
{
	struct fpm_http_gateway_s *gw = up->gw;
	const struct timeval *tv;

	if (!up->deadline) {
		return;
	}
	/* A connect still in progress keeps its own deadline: the request is already
	 * queued against this connection but nothing of it has reached the target, so
	 * it is the connect that is being waited for. */
	if (up->connecting) {
		tv = gw->upstream_connect_timeout_ms > 0 ? &gw->upstream_connect_timeout : NULL;
	} else if (up->busy) {
		/* A paused upstream (issue #596: http.response_buffer reached) is one the
		 * gateway chose not to read, so no progress cannot be observed and the
		 * timer must not run; fpm_http_response_resume() arms it again with a
		 * full interval. What bounds a client that never drains its buffer is
		 * http.write_timeout, on the client socket. */
		tv = (gw->upstream_read_timeout_ms > 0 && !(up->current && up->current->read_paused))
					 ? &gw->upstream_read_timeout
					 : NULL;
	} else {
		tv = NULL;
	}
	if (!tv) {
		fpm_http_upstream_deadline_stop(up);
		return;
	}
	event_add(up->deadline, tv);
}

void fpm_http_upstream_deadline_free(fpm_http_upstream *up)
{
	if (up->deadline) {
		event_del(up->deadline);
		event_free(up->deadline);
		up->deadline = NULL;
	}
}

#endif /* HAVE_FPM_HTTP */