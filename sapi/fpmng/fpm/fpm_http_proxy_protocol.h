/* fpm-ng: PROXY protocol v1 (text) and v2 (binary) header parser for the
 * HTTP gateway (issue #651).
 *
 * This file has no libevent and no logging, so tests/fuzz can include it
 * on its own. The socket side (reading the header before evhttp sees a byte,
 * the trusted-source check, the reject path) is in fpm_http_proxy.c.
 *
 * The parser answers one question per call: given the bytes received so far,
 * is the header complete, still incomplete, or malformed. Every byte of an
 * incomplete answer belongs to the header, so the caller may consume them
 * without reading past the header.
 */

#ifndef FPM_HTTP_PROXY_PROTOCOL_H
#define FPM_HTTP_PROXY_PROTOCOL_H 1

#include <stddef.h>
#include <netinet/in.h>

/* Largest v1 header, CRLF included. The specification fixes this at 107
 * bytes; a longer line without CRLF is refused. */
#define FPM_HTTP_PROXY_V1_MAX 107

/* Largest v2 address block after the 16-byte fixed part. The address itself
 * needs at most 36 bytes (IPv6). The rest is TLVs, which this code skips and
 * does not read, so the bound is a generous cap that stops a client from
 * making the gateway hold an arbitrarily long header. */
#define FPM_HTTP_PROXY_V2_BODY_MAX 512

/* Bytes the caller must be able to hold while the header is incomplete. */
#define FPM_HTTP_PROXY_MAX (16 + FPM_HTTP_PROXY_V2_BODY_MAX)

enum fpm_http_proxy_status {
	FPM_HTTP_PROXY_NEED_MORE, /* the header is a valid prefix; read more */
	FPM_HTTP_PROXY_DONE, /* the header is complete; see fpm_http_proxy_header_s */
	FPM_HTTP_PROXY_BAD /* not a PROXY header, or one that breaks a rule above */
};

struct fpm_http_proxy_header_s {
	size_t length; /* bytes of the header, from the first byte to its end */
	int has_source; /* 1: addr and port are the client; 0: LOCAL or UNKNOWN, keep the socket peer */
	char addr[INET6_ADDRSTRLEN]; /* canonical text of the source address, valid when has_source */
	unsigned short port; /* source port, valid when has_source */
};

/* Parses the header at the start of buf[0..len). On FPM_HTTP_PROXY_DONE, out
 * is filled and out->length is at most len. On NEED_MORE and BAD, out is
 * not written. */
enum fpm_http_proxy_status fpm_http_proxy_parse(const unsigned char *buf, size_t len, struct fpm_http_proxy_header_s *out);

#endif
