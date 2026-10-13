/* fpm-ng: http.request_id (issue #642) -- one correlation id per gateway
 * request, written to the access log as request_id=, sent to the target as
 * HTTP_X_REQUEST_ID (FastCGI) or X-Request-Id (HTTP route targets), and sent
 * back to the client as the X-Request-Id response header.
 *
 * Who decides the value is fpm_http_client_request_begin() in fpm_http.c,
 * with the trust rule of http.trusted_proxies (fpm_http_forwarded.h): an
 * inbound X-Request-Id is believed only from a direct peer in that list.
 * This file only generates and checks ids; it has no state.
 */

#ifndef FPM_HTTP_REQUEST_ID_H
#define FPM_HTTP_REQUEST_ID_H 1

#include <stdbool.h>

/* Longest inbound id that propagate accepts. A generated id is shorter. */
#define FPM_HTTP_REQUEST_ID_MAX 128
/* Buffer size for one id, including the terminating NUL. */
#define FPM_HTTP_REQUEST_ID_SIZE (FPM_HTTP_REQUEST_ID_MAX + 1)

/* 128 random bits from getentropy(), as 32 lowercase hex characters.
 * Returns false (and out is set to "") when the kernel refuses entropy;
 * the request then goes on without an id. */
bool fpm_http_request_id_generate(char out[FPM_HTTP_REQUEST_ID_SIZE]);

/* An inbound id is accepted only when it is 1 to FPM_HTTP_REQUEST_ID_MAX
 * characters from [A-Za-z0-9._-]. The strict set keeps the value safe in the
 * access log (no escaping needed) and in a header line (no CR/LF, no space).
 * Anything else is replaced by a generated id, never truncated. */
bool fpm_http_request_id_valid(const char *id);

#endif
