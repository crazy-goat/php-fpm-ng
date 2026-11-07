/* fpm-ng: WebSocket Upgrade passthrough on the gateway (issue #659).
 *
 * A route that opted in with http.route_upgrade[<prefix>] = websocket forwards
 * the handshake to a worker-executor http-direct target (fpm_http_client.c). When
 * the target answers 101, this file takes both connections over and copies bytes
 * between them until one side ends. Frames are never parsed: the target owns the
 * WebSocket codec (fpm_http_direct_ws.c), and the gateway only moves bytes.
 *
 * Why two bufferevents, not the raw fd the upstream uses: the client side already
 * is a bufferevent, and the upstream fd gets one here, so both directions have
 * the same watermarks, the same read pause and the same flush. The upstream
 * fd's own read event is removed before the bufferevent takes the fd
 * (fpm_http_upstream_release()).
 *
 * Why the client's callbacks are replaced, and the evhttp connection is kept:
 * the same reason as the worker side (fpm_http_direct_ws.c, the hijack comment).
 * evhttp_connection_free() shuts the fd down with SHUT_WR, and it would end the
 * tunnel the moment it started. evhttp keeps owning the bufferevent until
 * fpm_ws_teardown() frees the connection. The connection stays in the gateway's
 * client index and keeps its share of http.max_connections until then.
 *
 * Bounds: a side whose peer has FPM_GW_WS_HIGH bytes unsent stops being read.
 * The idle and total timers come from http.upgrade_idle_timeout and
 * http.upgrade_max_duration. A closed side flushes what it already sent to the
 * other side, and then the other side is shut down (SHUT_WR) and drained for a
 * linger, so a close frame or a TLS close_notify is not cut by an RST.
 *
 * All callbacks only record the cause and activate `step`. The bufferevents and
 * the connection are freed in fpm_ws_step(), a separate event, never inside a
 * callback of the bufferevent being freed (the use-after-free this avoids). */
#include "fpm_config.h"
#include "fpm_http.h"
#include "fpm_http_internal.h"
#include "fpm_http_direct_tls.h"

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* A side is paused above HIGH and resumed at LOW. Bytes in flight are bounded
 * by about HIGH per direction, plus one read. */
#define FPM_GW_WS_HIGH (64 * 1024)
#define FPM_GW_WS_LOW (16 * 1024)
/* The linger for a flush and for a drain, matching the worker's 5 s (#472):
 * long enough for a peer to answer a close, short enough that a silent peer
 * cannot hold the fd. */
#define FPM_GW_WS_LINGER_SECONDS 5

enum fpm_gw_ws_stage_e {
	FPM_GW_WS_RELAY, /* both directions open */
	FPM_GW_WS_FLUSH, /* upstream gone: the client's pending output is being sent */
	FPM_GW_WS_LINGER /* client shut down for writing, draining until its EOF */
};

struct fpm_gw_ws_s {
	struct fpm_http_gateway_s *gw;
	struct fpm_http_client_s *cl;
	struct evhttp_connection *evcon; /* the client, owned by evhttp */
	struct bufferevent *cbev; /* evcon's bufferevent, same lifetime */
	struct bufferevent *ubev; /* the upstream socket, owned here; NULL once freed */
	struct event *idle;
	struct event *total;
	struct event *deadline; /* flush and linger bound */
	struct event *step; /* the one place that frees (see the file comment) */
	enum fpm_gw_ws_stage_e stage;
	unsigned finishing : 1; /* no more bytes are copied */
	unsigned client_gone : 1; /* the client side ended; nothing to flush to it */
	unsigned timed_out : 1; /* the deadline of the current stage passed */
	struct fpm_gw_ws_s *prev, *next; /* gw->ws_relays */
};

static void fpm_gw_ws_kick(struct fpm_gw_ws_s *ws)
{
	event_active(ws->step, EV_TIMEOUT, 0);
}

static void fpm_gw_ws_touch(struct fpm_gw_ws_s *ws)
{
	if (!ws->finishing) {
		event_add(ws->idle, &ws->gw->upgrade_idle);
	}
}

/* Stops copying and asks the step to close. Either side's end, or a bound,
 * comes here. client_gone is sticky: once the client ended, a later event
 * from the upstream does not make the client a live peer again. */
static void fpm_gw_ws_finish(struct fpm_gw_ws_s *ws, int client_gone)
{
	if (client_gone) {
		ws->client_gone = 1;
	}
	if (!ws->finishing) {
		ws->finishing = 1;
		event_del(ws->idle);
		event_del(ws->total);
		if (ws->ubev) {
			bufferevent_setcb(ws->ubev, NULL, NULL, NULL, NULL);
		}
	}
	fpm_gw_ws_kick(ws);
}

static void fpm_gw_ws_move(struct fpm_gw_ws_s *ws, struct bufferevent *src, struct bufferevent *dst)
{
	struct evbuffer *out = bufferevent_get_output(dst);

	evbuffer_add_buffer(out, bufferevent_get_input(src));
	fpm_gw_ws_touch(ws);
	if (evbuffer_get_length(out) >= FPM_GW_WS_HIGH) {
		bufferevent_disable(src, EV_READ);
	}
}

static void fpm_gw_ws_readcb(struct bufferevent *bev, void *arg)
{
	struct fpm_gw_ws_s *ws = arg;

	if (ws->finishing) {
		return;
	}
	if (bev == ws->cbev) {
		fpm_gw_ws_move(ws, ws->cbev, ws->ubev);
	} else {
		fpm_gw_ws_move(ws, ws->ubev, ws->cbev);
	}
}

/* The writer's output fell to LOW: the other side may be read again. The TLS
 * layer can hold plaintext behind a paused read (fpm_http_direct_tls.h), so the
 * client side is pulled before the move. */
static void fpm_gw_ws_writecb(struct bufferevent *bev, void *arg)
{
	struct fpm_gw_ws_s *ws = arg;
	struct bufferevent *src = bev == ws->cbev ? ws->ubev : ws->cbev;

	if (ws->finishing || !src || evbuffer_get_length(bufferevent_get_output(bev)) > FPM_GW_WS_LOW) {
		return;
	}
	bufferevent_enable(src, EV_READ);
	if (src == ws->cbev) {
		fpm_http_direct_tls_pull_pending(src, bufferevent_get_input(src));
	}
	fpm_gw_ws_move(ws, src, bev);
}

static void fpm_gw_ws_eventcb(struct bufferevent *bev, short what, void *arg)
{
	struct fpm_gw_ws_s *ws = arg;

	if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
		fpm_gw_ws_finish(ws, bev == ws->cbev);
	}
}

static void fpm_gw_ws_idle_cb(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_gw_ws_s *ws = arg;

	(void) fd;
	(void) what;
	fpm_gw_ws_finish(ws, 0);
}

static void fpm_gw_ws_deadline_cb(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_gw_ws_s *ws = arg;

	(void) fd;
	(void) what;
	ws->timed_out = 1;
	fpm_gw_ws_kick(ws);
}

/* Removes the client from the gateway (its gauge, its index entry and its
 * connection node) and frees the evhttp connection. The request evhttp still
 * holds is freed with it: it was never answered. */
static void fpm_gw_ws_release_client(struct fpm_gw_ws_s *ws)
{
	bufferevent_setcb(ws->cbev, NULL, NULL, NULL, NULL);
	fpm_http_client_closed(ws->evcon, ws->cl);
	evhttp_connection_free(ws->evcon);
}

static void fpm_gw_ws_teardown(struct fpm_gw_ws_s *ws)
{
	if (ws->prev) {
		ws->prev->next = ws->next;
	} else {
		ws->gw->ws_relays = ws->next;
	}
	if (ws->next) {
		ws->next->prev = ws->prev;
	}
	if (ws->ubev) {
		bufferevent_free(ws->ubev);
	}
	if (ws->idle) {
		event_free(ws->idle);
	}
	if (ws->total) {
		event_free(ws->total);
	}
	if (ws->deadline) {
		event_free(ws->deadline);
	}
	fpm_gw_ws_release_client(ws);
	event_free(ws->step);
	free(ws);
}

static void fpm_gw_ws_flush_writecb(struct bufferevent *bev, void *arg)
{
	(void) bev;
	fpm_gw_ws_kick(arg);
}

static void fpm_gw_ws_linger_read(struct bufferevent *bev, void *arg)
{
	evbuffer_drain(bufferevent_get_input(bev), evbuffer_get_length(bufferevent_get_input(bev)));
	(void) arg;
}

static void fpm_gw_ws_stage_eventcb(struct bufferevent *bev, short what, void *arg)
{
	struct fpm_gw_ws_s *ws = arg;

	(void) bev;
	if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
		ws->client_gone = 1;
		fpm_gw_ws_kick(ws);
	}
}

static void fpm_gw_ws_arm_deadline(struct fpm_gw_ws_s *ws)
{
	struct timeval linger = { FPM_GW_WS_LINGER_SECONDS, 0 };

	ws->timed_out = 0;
	event_del(ws->deadline);
	event_add(ws->deadline, &linger);
}

/* The client gets no more bytes from the upstream. Its pending output is sent
 * first, bounded by the linger. The write watermark is 0, so writecb runs when
 * the output is empty. */
static void fpm_gw_ws_enter_flush(struct fpm_gw_ws_s *ws)
{
	ws->stage = FPM_GW_WS_FLUSH;
	bufferevent_setwatermark(ws->cbev, EV_WRITE, 0, 0);
	bufferevent_setcb(ws->cbev, NULL, fpm_gw_ws_flush_writecb, fpm_gw_ws_stage_eventcb, ws);
	bufferevent_disable(ws->cbev, EV_READ);
	fpm_gw_ws_arm_deadline(ws);
}

/* The client has nothing left to receive. Its write side is shut, so the peer
 * sees our end, and its remaining bytes are read and dropped until its EOF or
 * the linger. An unread byte at close() would send an RST that can cut our
 * final bytes (see fpm_ws_release() in fpm_http_direct_ws.c, #472). */
static void fpm_gw_ws_enter_linger(struct fpm_gw_ws_s *ws)
{
	if (ws->client_gone) {
		fpm_gw_ws_teardown(ws);
		return;
	}
	ws->stage = FPM_GW_WS_LINGER;
	shutdown(bufferevent_getfd(ws->cbev), SHUT_WR);
	bufferevent_setcb(ws->cbev, fpm_gw_ws_linger_read, NULL, fpm_gw_ws_stage_eventcb, ws);
	bufferevent_enable(ws->cbev, EV_READ);
	fpm_gw_ws_arm_deadline(ws);
}

static void fpm_gw_ws_step(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_gw_ws_s *ws = arg;

	(void) fd;
	(void) what;
	if (!ws->finishing) {
		return;
	}
	if (ws->ubev) {
		bufferevent_free(ws->ubev);
		ws->ubev = NULL;
	}
	switch (ws->stage) {
		case FPM_GW_WS_RELAY:
			if (ws->client_gone || evbuffer_get_length(bufferevent_get_output(ws->cbev)) == 0) {
				fpm_gw_ws_enter_linger(ws);
			} else {
				fpm_gw_ws_enter_flush(ws);
			}
			return;
		case FPM_GW_WS_FLUSH:
			if (ws->client_gone) {
				fpm_gw_ws_teardown(ws);
			} else if (ws->timed_out || evbuffer_get_length(bufferevent_get_output(ws->cbev)) == 0) {
				fpm_gw_ws_enter_linger(ws);
			}
			return;
		case FPM_GW_WS_LINGER:
			if (ws->client_gone || ws->timed_out) {
				fpm_gw_ws_teardown(ws);
			}
			return;
	}
}

/* Called by fpm_http_client.c when the upstream answered 101 (issue #659). The
 * head (the status line and headers, blank line included) and any bytes that
 * followed it in the same read are copied to the client, so the handshake
 * reaches the client exactly as the target sent it. Returns 0 when the relay
 * owns both connections and the upstream struct is released, or -1 with nothing
 * changed; the caller then fails the upstream as usual.
 *
 * Nothing that can fail runs after fpm_http_upstream_release(), except the
 * allocations this function makes before it. */
int fpm_http_gw_ws_start(fpm_http_upstream *up, const char *head, size_t head_len,
		const char *tail, size_t tail_len)
{
	fpm_http_conn *c = up->current;
	struct fpm_http_gateway_s *gw = up->gw;
	struct fpm_http_client_s *cl = c ? c->client : NULL;
	struct evhttp_connection *evcon = c ? c->evcon : NULL;
	struct bufferevent *cbev = evcon ? evhttp_connection_get_bufferevent(evcon) : NULL;
	struct bufferevent *ubev;
	struct fpm_gw_ws_s *ws;

	if (!cl || !cbev || up->fd < 0) {
		return -1;
	}
	ws = calloc(1, sizeof(*ws));
	if (!ws) {
		return -1;
	}
	ws->idle = event_new(gw->base, -1, 0, fpm_gw_ws_idle_cb, ws);
	ws->total = event_new(gw->base, -1, 0, fpm_gw_ws_idle_cb, ws);
	ws->deadline = event_new(gw->base, -1, 0, fpm_gw_ws_deadline_cb, ws);
	ws->step = event_new(gw->base, -1, 0, fpm_gw_ws_step, ws);
	/* The socket is taken last: until the bufferevent exists, a failure leaves the
	 * fd with the upstream, which fails as usual. */
	ubev = (ws->idle && ws->total && ws->deadline && ws->step)
				   ? bufferevent_socket_new(gw->base, up->fd, BEV_OPT_CLOSE_ON_FREE)
				   : NULL;
	if (!ubev) {
		if (ws->idle) {
			event_free(ws->idle);
		}
		if (ws->total) {
			event_free(ws->total);
		}
		if (ws->deadline) {
			event_free(ws->deadline);
		}
		if (ws->step) {
			event_free(ws->step);
		}
		free(ws);
		return -1;
	}
	ws->gw = gw;
	ws->cl = cl;
	ws->evcon = evcon;
	ws->cbev = cbev;
	ws->ubev = ubev;

	/* From here nothing can fail. The client's keep-alive timers go: they belong
	 * to a connection that waits for its next request. */
	if (cl->ka_timer) {
		event_free(cl->ka_timer);
		cl->ka_timer = NULL;
	}
	if (cl->ka_watch) {
		event_free(cl->ka_watch);
		cl->ka_watch = NULL;
	}
	if (cl->close_timer) {
		event_free(cl->close_timer);
		cl->close_timer = NULL;
	}

	/* The request is answered by the upstream's bytes, not by evhttp, so the
	 * fpm_http_conn goes now. Its client node and the connection stay. */
	c->upstream = NULL;
	up->current = NULL;
	c->evcon = NULL;
	fpm_http_conn_free(c);

	ws->prev = NULL;
	ws->next = gw->ws_relays;
	if (gw->ws_relays) {
		gw->ws_relays->prev = ws;
	}
	gw->ws_relays = ws;

	bufferevent_write(cbev, head, head_len);
	if (tail_len > 0) {
		bufferevent_write(cbev, tail, tail_len);
	}

	bufferevent_setcb(cbev, fpm_gw_ws_readcb, fpm_gw_ws_writecb, fpm_gw_ws_eventcb, ws);
	bufferevent_setwatermark(cbev, EV_READ, 0, FPM_GW_WS_HIGH);
	bufferevent_setwatermark(cbev, EV_WRITE, FPM_GW_WS_LOW, 0);
	bufferevent_set_timeouts(cbev, NULL, NULL);
	bufferevent_enable(cbev, EV_READ | EV_WRITE);

	bufferevent_setcb(ubev, fpm_gw_ws_readcb, fpm_gw_ws_writecb, fpm_gw_ws_eventcb, ws);
	bufferevent_setwatermark(ubev, EV_READ, 0, FPM_GW_WS_HIGH);
	bufferevent_setwatermark(ubev, EV_WRITE, FPM_GW_WS_LOW, 0);
	bufferevent_enable(ubev, EV_READ | EV_WRITE);

	event_add(ws->total, &gw->upgrade_max);
	fpm_gw_ws_touch(ws);

	/* The socket is the bufferevent's from here: the upstream struct must not
	 * close it. The slot goes back, and a request waiting for one can take it. */
	fpm_http_upstream_release(up);
	fpm_http_pump(gw);
	return 0;
}

/* The gateway's teardown (fpm_http_gw_proc.c), before its event loop is freed.
 * No callback runs here, so each relay is torn down directly. */
void fpm_http_gw_ws_close_all(struct fpm_http_gateway_s *gw)
{
	struct fpm_gw_ws_s *ws = gw->ws_relays;

	while (ws) {
		struct fpm_gw_ws_s *next = ws->next;

		fpm_gw_ws_teardown(ws);
		ws = next;
	}
}
