/* fpm-ng: PROXY protocol on the gateway's TLS and main listener (issue #651).
 *
 * The header is read from the raw socket before evhttp or the TLS bufferevent
 * sees a byte. It is read by a priority-0 event on the connection's fd, while
 * bufferevent read is disabled. The byte-level format is in
 * fpm_http_proxy_protocol.h; the connection state is on
 * struct fpm_http_read_deadline_s (fpm_http_internal.h).
 */

#ifndef FPM_HTTP_PROXY_H
#define FPM_HTTP_PROXY_H 1

struct fpm_http_read_deadline_s;
struct fpm_http_gateway_s;
struct fpm_http_client_s;
struct evhttp_connection;

/* Called on the first loop pass of a connection when proxy_pending is set. It
 * checks the socket peer against http.proxy_protocol_trusted, then starts the
 * header read. */
void fpm_http_proxy_start(struct fpm_http_read_deadline_s *dl);

/* Read callback of the header reader (ev_eof while proxy_pending). It reads
 * the header with MSG_PEEK and consumes only the header bytes. */
void fpm_http_proxy_read(int fd, short what, void *arg);

/* Copies the accepted header from the connection's deadline node into cl, when
 * it has not been copied yet. Returns 1 if cl holds an accepted header. */
int fpm_http_proxy_admit(struct fpm_http_gateway_s *gw, struct fpm_http_client_s *cl, struct evhttp_connection *evcon);

/* Closes a connection that has no accepted PROXY header. The socket is shut
 * down at once; the connection is freed on the next loop pass, after the
 * request callback has returned (see fpm_http_proxy.c). */
void fpm_http_proxy_refuse(struct fpm_http_gateway_s *gw, struct fpm_http_client_s *cl, struct evhttp_connection *evcon);

#endif
