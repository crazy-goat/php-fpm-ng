/* fpm-ng: the host name of a gateway request, for host-bound http.route[]
 * entries (issue #658).
 *
 * This file only parses. The table and the matching are in fpm_http_route.c
 * and fpm_http.c. A malformed value never produces a name, so it can never
 * select a host-bound route. */

#ifndef FPM_HTTP_HOST_H
#define FPM_HTTP_HOST_H 1

#include <stddef.h>

struct evhttp_request;

/* 1 when name is a valid configuration host name, else 0. The grammar is
 * dot-separated labels of ASCII letters, digits and '-'. A label is 1 to 63
 * bytes, does not begin or end with '-', and the name is at most 253 bytes.
 * There is no port, no brackets (IPv6), no wildcard and no trailing dot. */
int fpm_http_host_valid_name(const char *name, size_t len);

/* Lowercased copy of a name that fpm_http_host_valid_name() accepted, or NULL
 * when out of memory. The caller frees it. */
char *fpm_http_host_dup_lower(const char *name, size_t len);

/* 1 when the first len bytes of a and b are equal ignoring ASCII case, else 0.
 * Same fold as fpm_http_host_dup_lower(), so duplicate checks and routing agree. */
int fpm_http_host_equal(const char *a, const char *b, size_t len);

/* Parses one Host value (RFC 9110 7.2: uri-host [ ":" port ]). On success the
 * lowercased host name without its port is written to out, NUL-terminated, and
 * 1 is returned. Returns 0 for every value that cannot name a configured host:
 * empty, an IPv6 literal, a port that is empty, not decimal, or above 65535, or
 * a name that fails fpm_http_host_valid_name(). Surrounding spaces and tabs are
 * trimmed first. */
int fpm_http_host_parse(const char *value, size_t len, char *out, size_t out_size);

/* The host name of a request, as the gateway routes on it. An absolute-form
 * target names its own authority, which replaces Host (RFC 9112 3.2.2, #534).
 * Otherwise the request must carry exactly one Host header; zero or several
 * Host headers give 0 (no name). Returns 1 with out filled, or 0. */
int fpm_http_host_of_request(struct evhttp_request *req, char *out, size_t out_size);

#endif
