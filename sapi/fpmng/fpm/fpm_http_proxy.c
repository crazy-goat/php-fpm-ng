/* fpm-ng: PROXY protocol on the gateway's TLS and main listener (issue #651).
 * See fpm_http_proxy.h for the overview and fpm_http_proxy_protocol.h for the
 * byte format. */
#include "fpm_config.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/bufferevent.h>

#include "fpm_http_internal.h"
#include "fpm_http_acl.h"
#include "fpm_http_proxy.h"
#include "fpm_http_proxy_protocol.h"

/* The socket peer as text, the form fpm_http_acl_check() takes. Returns 0 for
 * an address family the gateway does not serve. */
static int fpm_http_proxy_peer_text(const struct sockaddr_storage *ss, char *out, size_t outlen)
{
	if (ss->ss_family == AF_INET) {
		return inet_ntop(AF_INET, &((const struct sockaddr_in *) ss)->sin_addr, out, outlen) != NULL;
	}
	if (ss->ss_family == AF_INET6) {
		return inet_ntop(AF_INET6, &((const struct sockaddr_in6 *) ss)->sin6_addr, out, outlen) != NULL;
	}
	return 0;
}

/* The first loop pass of the connection. The bufferevent read is disabled
 * here, before evhttp or the TLS layer can read the fd. The check runs on the
 * socket peer, not on anything the client sent, so an untrusted source is
 * refused before one byte of it is consumed. */
void fpm_http_proxy_start(struct fpm_http_read_deadline_s *dl)
{
	struct bufferevent *bev = dl->bev;
	evutil_socket_t fd = bufferevent_getfd(bev);
	struct sockaddr_storage ss;
	socklen_t sslen = sizeof(ss);
	char text[INET6_ADDRSTRLEN];

	/* A NULL ACL means "no restriction" to fpm_http_acl_check(), so it is
	 * refused here as well; -t never lets an empty list through. */
	if (fd < 0 || !dl->gw->proxy_trusted_acl || getpeername(fd, (struct sockaddr *) &ss, &sslen) != 0 || !fpm_http_proxy_peer_text(&ss, text, sizeof(text)) || !fpm_http_acl_check(dl->gw->proxy_trusted_acl, text)) {
		/* fd < 0 should not happen; every case closes the connection */
		fpm_http_read_deadline_proxy_done(dl, 0);
		return;
	}
	dl->fd = fd;
	dl->proxy_pending = 1;
	dl->proxy_len = 0;
	dl->proxy_enabled = bufferevent_get_enabled(bev) & (EV_READ | EV_WRITE);
	/* Both directions: with EV_WRITE left on, a TLS handshake write event would
	 * run SSL_do_handshake() over the PROXY bytes (see the handshake callbacks in
	 * libevent 2.1.12 bufferevent_openssl.c). */
	bufferevent_disable(bev, EV_READ | EV_WRITE);
	event_del(dl->ev_eof);
	event_assign(dl->ev_eof, dl->gw->base, fd, EV_READ | EV_PERSIST, fpm_http_proxy_read, dl);
	event_priority_set(dl->ev_eof, 0);
	/* No timeout here: a persistent event with one fires on every expiry, so
	 * a silent client would spin the loop. The read deadline (dl->ev) bounds
	 * the wait instead. */
	event_add(dl->ev_eof, NULL);
}

/* The header reader. Bytes are peeked first and consumed only once the parser
 * says what they are: NEED_MORE bytes all belong to the header, so they are
 * taken into proxy_buf; DONE consumes exactly the rest of the header, so the
 * HTTP request behind it (same segment or later) stays in the kernel for
 * evhttp. */
void fpm_http_proxy_read(int fd, short what, void *arg)
{
	struct fpm_http_read_deadline_s *dl = arg;
	struct fpm_http_proxy_header_s hdr;
	unsigned char *at = dl->proxy_buf + dl->proxy_len;
	size_t room = FPM_HTTP_PROXY_MAX - dl->proxy_len;
	size_t total, need;
	ssize_t n;
	enum fpm_http_proxy_status st;

	(void) what;
	n = recv(fd, at, room, MSG_PEEK | MSG_DONTWAIT);
	if (n < 0 && fpm_http_would_block(errno)) {
		return; /* no bytes yet; the read deadline still covers the wait */
	}
	if (n <= 0) {
		/* closed or failed before the header was complete */
		fpm_http_read_deadline_proxy_done(dl, 0);
		return;
	}
	total = dl->proxy_len + (size_t) n;
	st = fpm_http_proxy_parse(dl->proxy_buf, total, &hdr);
	if (st == FPM_HTTP_PROXY_BAD) {
		fpm_http_read_deadline_proxy_done(dl, 0);
		return;
	}
	if (st == FPM_HTTP_PROXY_NEED_MORE) {
		if (recv(fd, at, (size_t) n, MSG_DONTWAIT) != n) {
			fpm_http_read_deadline_proxy_done(dl, 0);
			return;
		}
		dl->proxy_len = total;
		return;
	}
	/* DONE. The parser never reports a header shorter than the bytes it already
	 * said were incomplete, so need is at least 1; a zero would mean a parser
	 * bug, and the connection is refused rather than read past the header. */
	if (hdr.length <= dl->proxy_len || hdr.length > total) {
		fpm_http_read_deadline_proxy_done(dl, 0);
		return;
	}
	need = hdr.length - dl->proxy_len;
	if (recv(fd, at, need, MSG_DONTWAIT) != (ssize_t) need) {
		fpm_http_read_deadline_proxy_done(dl, 0);
		return;
	}
	dl->proxy = hdr;
	dl->proxy_len = 0;
	dl->proxy_accepted = 1;
	fpm_http_read_deadline_proxy_done(dl, 1);
}

/* The record is kept on the connection's client node, which lives as long as
 * the connection (fpm_http_client_track()). The deadline node is freed at the
 * first request (fpm_http_read_deadline_disarm()), so the copy has to happen
 * before that, in fpm_http_request(). */
int fpm_http_proxy_admit(struct fpm_http_gateway_s *gw, struct fpm_http_client_s *cl, struct evhttp_connection *evcon)
{
	struct bufferevent *bev;
	struct fpm_http_read_deadline_s *dl;

	if (!cl || !evcon) {
		return 0;
	}
	if (cl->proxy_done) {
		return 1;
	}
	bev = evhttp_connection_get_bufferevent(evcon);
	for (dl = gw->deadlines; dl; dl = dl->next) {
		if (dl->bev == bev) {
			if (!dl->proxy_on || dl->proxy_pending || !dl->proxy_accepted) {
				return 0;
			}
			cl->proxy = dl->proxy;
			cl->proxy_done = 1;
			return 1;
		}
	}
	return 0;
}

/* The close scheduled by fpm_http_proxy_refuse(). It runs after the request
	 * callback has returned: evhttp_connection_free() frees the request that the
	 * callback is still handling, so it cannot run inside it. The close callback
	 * (fpm_http_client_closed()) frees this timer, so cl and cl->evcon are valid
	 * here. */
static void fpm_http_proxy_refused_fire(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_http_client_s *cl = arg;

	(void) fd;
	(void) what;
	evhttp_connection_free(cl->evcon);
}

/* Closes a connection whose PROXY header was refused (issue #651, review R1).
 * Before this, the request evhttp had already dispatched stayed on the
 * connection and nothing closed it: the connection, the request and the fd
 * leaked, and fpm_http_client_track() kept counting the connection in
 * connections_open. The deadline node goes first: it holds the bufferevent
 * reference that would keep the fd open after evhttp frees the connection,
 * and it watches the fd. cl->proxy_refused keeps a second request on the same
 * connection from scheduling a second close. A NULL cl (OOM in
 * fpm_http_client_track()) gets only the shutdown; the timer's OOM leaves the
 * connection open as before. */
void fpm_http_proxy_refuse(struct fpm_http_gateway_s *gw, struct fpm_http_client_s *cl, struct evhttp_connection *evcon)
{
	struct bufferevent *bev = evcon ? evhttp_connection_get_bufferevent(evcon) : NULL;
	evutil_socket_t fd = bev ? bufferevent_getfd(bev) : -1;
	struct timeval now = { 0, 0 };

	if (fd >= 0) {
		shutdown(fd, SHUT_RDWR);
	}
	if (!cl || cl->proxy_refused) {
		return;
	}
	cl->proxy_refused = 1;
	fpm_http_read_deadline_disarm(gw, bev);
	if (!cl->close_timer) {
		cl->close_timer = event_new(gw->base, -1, EV_TIMEOUT, fpm_http_proxy_refused_fire, cl);
	}
	if (cl->close_timer) {
		event_add(cl->close_timer, &now);
	}
}
