/* fpm-ng: the replies the gateway writes itself for an error (issue #882).
 * See fpm_http_error.c for the rules these functions keep. */

#ifndef FPM_HTTP_ERROR_H
#define FPM_HTTP_ERROR_H 1

#include <stddef.h>

struct evhttp_request;

/* Adds the headers that every gateway reply with `code` carries on top of
 * the ones the caller set: Retry-After on every 503. Does not clear the
 * output headers and does not touch X-Request-Id. */
void fpm_http_reply_headers(struct evhttp_request *req, int code);

/* Prepares the error reply `code` on req: clears the output headers, keeps
 * the X-Request-Id the request already has, then adds Content-Type,
 * Connection: close and fpm_http_reply_headers(). Returns the body bytes the
 * reply counts for the access log: the length of the body that it puts in the
 * output buffer of req, "<reason>\n", and 0 for HEAD. It allocates nothing, so
 * the count is what is sent. Call it before the access log line, and call
 * fpm_http_error_send() last. */
size_t fpm_http_error_prepare(struct evhttp_request *req, int code, const char *reason);

/* Sends the error reply prepared by fpm_http_error_prepare(), with the body
 * that call left in the output buffer (none for HEAD). Never call evhttp_send_error() on a gateway
 * reply: it clears the output headers, and with them X-Request-Id. The send
 * may end the request, so nothing may read req after this call. */
void fpm_http_error_send(struct evhttp_request *req, int code, const char *reason);

/* Prepare and send in one call, for a reply that has no access log line of its
 * own to write before the send. The byte count prepare returns is discarded. */
void fpm_http_error_reply(struct evhttp_request *req, int code, const char *reason);

#endif /* FPM_HTTP_ERROR_H */
