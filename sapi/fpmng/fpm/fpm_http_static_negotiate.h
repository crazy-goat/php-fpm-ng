/* Content negotiation and Cache-Control for static files (issue #648): which
 * precompressed sidecar a client may be sent, and which Cache-Control value a
 * path gets. Pure functions over the request header and the pool's rules.
 */

#ifndef FPM_HTTP_STATIC_NEGOTIATE_H
#define FPM_HTTP_STATIC_NEGOTIATE_H 1

struct key_value_s;

/* 1 when `coding` (a content-coding name, lower case) is listed in the
 * Accept-Encoding value `accept` with a non-zero q-value, 0 otherwise. A
 * wildcard does not count: a client has to name the coding it accepts. */
int fpm_http_static_accepts(const char *accept, const char *coding);

/* The Cache-Control value of the longest prefix in `rules` that `path` starts
 * with, or NULL when none matches. The rules are the http.static.cache_control[]
 * entries, each key a path prefix that begins with '/'. */
const char *fpm_http_static_cache_control_for(const struct key_value_s *rules, const char *path);

#endif
