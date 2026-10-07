/* fpm-ng: the request-target contract shared by the two ingresses.
 *
 * RFC 9112 3.2.2 lets a client send an absolute-form request-target
 * ("GET http://host/path HTTP/1.1"), and 3.2.1 makes a target that starts with
 * "/" origin-form whatever follows it -- so "//api/users" is the path
 * "//api/users", not the host "api" plus "/users". libevent's own parse reads
 * both the other way round, which is why the gateway reduced the target at its
 * ingress (#534) and why http-direct does the same (#681): once every consumer
 * -- ping.path, the operator namespace, access.suppress_path[], the static
 * lookup, REQUEST_URI, the access log -- reads one path, nothing can be matched
 * on one path and served on another.
 *
 * The gateway's copy is fpm_http_normalize_target(), called first in
 * fpm_http_request() and fpm_http_plain_request() (fpm_http.c), and the rest of
 * this API is what the callers read after it. The implementations live in
 * fpm_http_fcgi.c, where they were written for the gateway; this header exists
 * so http-direct can use them without including fpm_http_internal.h, which is
 * the gateway's private surface and carries every gateway struct with it.
 */

#ifndef FPM_HTTP_TARGET_H
#define FPM_HTTP_TARGET_H 1

#include <stddef.h>

#include "zend_smart_str.h"

struct evhttp_request;

/* Buffer size for fpm_http_absolute_authority(): a 253-byte DNS name, ":65535"
 * (6 bytes) and the NUL, rounded up (a longer authority is answered 400). */
#define FPM_HTTP_AUTHORITY_MAX 262

/* Rewrites libevent's parse of a request-target that starts with "//" so the
 * path is the raw one up to "?" or "#" and the host is dropped. A target with a
 * scheme keeps libevent's reading of an authority. Safe on any request: a target
 * that needs no change is left exactly as it is. */
void fpm_http_normalize_target(struct evhttp_request *req);

/* The path every consumer matches on, from that one parse. An empty path is "/"
 * only when an authority was present ("GET http://h", RFC 9112 3.2.2). */
const char *fpm_http_request_path(struct evhttp_request *req);

/* The same path copied out, query string already cut off, raw (no
 * percent-decoding). Returns the length, or 0 when there is no path or it does
 * not fit path_size-1 bytes (#534). The shape ping.path and access.suppress_path[]
 * are compared in. */
size_t fpm_http_raw_path(struct evhttp_request *req, char *path, size_t path_size);

/* The origin-form target (path and "?query") the FastCGI transport forwards as
 * REQUEST_URI and an http.route[] target receives on its request line, built
 * from the same parse, so the path a request is matched and routed on is the
 * path the application sees, for every form. The fragment is dropped (#534,
 * #462). A target libevent did not parse, or "*", is copied verbatim. */
void fpm_http_origin_form(smart_str *out, struct evhttp_request *req);

/* Copies the absolute-form authority of `uri` into `buf`: 1 = copied, 0 = `uri`
 * is not absolute-form or the authority is empty (the Host header stays), -1 =
 * it does not fit (the callers answer 400). The userinfo is not part of a Host
 * value (RFC 9110 7.2), so it is stripped. */
int fpm_http_absolute_authority(const char *uri, char *buf, size_t buf_len);

#endif /* FPM_HTTP_TARGET_H */