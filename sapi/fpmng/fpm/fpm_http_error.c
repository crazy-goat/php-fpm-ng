/* fpm-ng: the replies the gateway writes itself for an error (issue #882).
 *
 * evhttp_send_error() cannot be used for these replies. It clears every output
 * header (libevent's evhttp_send_page_()), and with them the X-Request-Id that
 * fpm_http_client_request_begin() put on the request, so an error reply went
 * out without the id the access log line names. Issue #858 moved the 502 off
 * evhttp_send_error() for the same reason; this file does that for every other
 * gateway error. The reply keeps the same status, reason and Connection: close
 * as before. Its body is "<reason>\n" as text/plain, the body of the 502 (#858),
 * and not libevent's HTML page, whose length the gateway cannot read back for
 * the access log. */

#include "fpm_config.h"

#include "fpm_http_error.h"

#ifdef HAVE_FPM_HTTP

#include <stdio.h>
#include <string.h>

#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include "fpm_http_internal.h"
#include "fpm_http_reply_policy.h"
#include "fpm_http_request_id.h"

/* Retry-After on every 503 the gateway writes, not only on the pool-full one.
 * RFC 9110 (15.6.4) lets the server send it with a 503 and does not require
 * it. The gateway sends it everywhere because the client gets the same
 * answer from every 503 this process writes: the condition is transient, and
 * a client that retries after one second is not made worse by it. The 503s
 * are the over-cap refusal, the drain and ready answers, the operator
 * refusals and the pool-full answer (fpm_http_reject_queued(), which sets it
 * by hand). Putting the rule here keeps the sites from drifting apart. */
void fpm_http_reply_headers(struct evhttp_request *req, int code)
{
	if (code == FPM_HTTP_SERVICE_UNAVAIL) {
		evhttp_add_header(evhttp_request_get_output_headers(req), "Retry-After", FPM_HTTP_RETRY_AFTER);
	}
}

size_t fpm_http_error_prepare(struct evhttp_request *req, int code, const char *reason)
{
	struct evkeyvalq *out = evhttp_request_get_output_headers(req);
	char id[FPM_HTTP_REQUEST_ID_SIZE] = "";
	const char *kept = evhttp_find_header(out, "X-Request-Id");
	const struct fpm_http_reply_policy_s *policy = fpm_http_reply_policy_installed();
	const struct fpm_http_error_page_s *page = fpm_http_reply_policy_page(policy, code);
	/* Issue #650: the configured headers are on the reply only when the
	 * request came in on the main listener (fpm_http_gencb adds them). The plain
	 * HTTP companion never has them, and its replies, which come through here
	 * too, must not get them. The first header stands for all: they are added
	 * together. */
	int keep_headers = policy && policy->nheaders && evhttp_find_header(out, policy->headers[0].name);

	/* The id is copied first: evhttp_clear_headers() frees the value it points
	 * into. An empty id means the request has none (http.request_id = off, or
	 * an http-direct target, which makes no id of its own). */
	if (kept) {
		snprintf(id, sizeof(id), "%s", kept);
	}
	evhttp_clear_headers(out);
	if (id[0]) {
		evhttp_add_header(out, "X-Request-Id", id);
	}
	if (keep_headers) {
		fpm_http_reply_headers_add(policy, out);
	}
	fpm_http_reply_headers(req, code);
	if (!page) {
		evhttp_add_header(out, "Content-Type", "text/plain");
	}
	/* evhttp_send_error() forced Connection: close on every reply it wrote.
	 * Callers that need the connection closed (the over-cap and authority
	 * refusals) depend on it, so it stays. */
	evhttp_add_header(out, "Connection", "close");

	/* A HEAD reply gets no body: libevent writes whatever body a HEAD reply has
	 * (2.1.12, http.c:628-633 in evhttp_make_header()). The access log counts
	 * what goes on the wire, as the 502 does (issue #858). */
	if (evhttp_request_get_command(req) == EVHTTP_REQ_HEAD) {
		/* Issue #650: a HEAD reply has the Content-Type of the page and no body. */
		if (page) {
			(void) fpm_http_error_page_put(page, req);
		}
		return 0;
	}
	if (page) {
		/* Issue #650: the page replaces "<reason>\n". It is added by reference,
		 * so the reply copies nothing, and the count is the length of the output
		 * buffer as for the plain body. */
		return fpm_http_error_page_put(page, req);
	}
	/* The body goes into the request's own output buffer, which libevent
	 * allocated with the request, so no separate buffer is allocated here (several
	 * callers are out-of-memory paths themselves). evbuffer_add_printf() can still
	 * fail under out-of-memory. The count is the length of what the buffer holds,
	 * so it is what is sent: 0 when the body could not be added (the rule of
	 * issue #967). */
	evbuffer_add_printf(evhttp_request_get_output_buffer(req), "%s\n", reason);
	return evbuffer_get_length(evhttp_request_get_output_buffer(req));
}

void fpm_http_error_send(struct evhttp_request *req, int code, const char *reason)
{
	/* The body, if any, is already in the output buffer (see prepare). Sent
	 * last: the send may end the request, and with it the connection. */
	evhttp_send_reply(req, code, reason, NULL);
}

void fpm_http_error_reply(struct evhttp_request *req, int code, const char *reason)
{
	(void) fpm_http_error_prepare(req, code, reason);
	fpm_http_error_send(req, code, reason);
}

#endif /* HAVE_FPM_HTTP */
