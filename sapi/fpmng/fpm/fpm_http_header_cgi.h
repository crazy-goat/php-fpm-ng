/* fpm-ng: one request-header-name -> CGI-key mapping for both HTTP transports.
 *
 * The gateway (fpm_http_build_request(), fpm_http_fcgi.c) and HTTP-direct
 * (fpm_http_direct_build_env(), fpm_http_direct_request.c) derived HTTP_*
 * keys from the same rules since issue #109, but each carried its own copy,
 * and the copies had already drifted twice over which headers get a key: the
 * Proxy exclusion and the name-length bound landed in one copy first (issue
 * #115), the underscore drop in both by hand (issue #595). One implementation
 * removes that drift by construction (issue #630): the skip rules, the length
 * bound and the upper-casing live here, and both builders only decide what to
 * do with the answer.
 *
 * The one genuine difference between the callers stays at the call sites, as
 * a parameter: the gateway maps "Content-Type" to CONTENT_TYPE inline (its
 * no-prefix branch), while HTTP-direct emits CONTENT_TYPE once before the
 * loop from evhttp_find_header() -- first match wins -- and skips every
 * Content-Type line inside it. With a single Content-Type header both
 * transports produce the same CONTENT_TYPE; duplicate Content-Type headers
 * differ by construction (first-wins against last-wins through fcgi_hash_set)
 * and this helper does not unify that.
 */

#ifndef FPM_HTTP_HEADER_CGI_H
#define FPM_HTTP_HEADER_CGI_H 1

#include <stdbool.h>
#include <stddef.h>

#include "fpm_http_direct_request.h" /* FPM_HTTP_HEADER_NAME_MAX */

/* "HTTP_" + the longest mapped name + NUL: the key buffer size at both call
 * sites. */
#define FPM_HTTP_HEADER_CGI_KEY_LEN (FPM_HTTP_HEADER_NAME_MAX + sizeof("HTTP_"))

/* Maps one request header name to the CGI key it is exported under. Returns 0
 * with `out` holding the NUL-terminated key, 1 when the header gets no key
 * (Content-Length, Proxy, a name containing '_', or Content-Type with
 * content_type_skip), and -1 when the name is above FPM_HTTP_HEADER_NAME_MAX
 * or `out` cannot hold the key -- both mean refuse, never truncate: a
 * truncated key could collide two headers into one. `out` must therefore hold
 * FPM_HTTP_HEADER_CGI_KEY_LEN bytes; both callers declare exactly that, so -1
 * for size is unreachable-but-safe. The length check runs first, so an
 * over-long name is refused even when it would otherwise be skipped, matching
 * both callers' up-front scans. */
int fpm_http_header_cgi_key(const char *name, bool content_type_skip, char *out, size_t out_size);

#endif
