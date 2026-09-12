/* fpm-ng: the raw HTTP server behind every operator endpoint.
 *
 * There is exactly one of these in the tree and it is deliberate. It started
 * as the accept loop inside fpm_pool_status.c (pool.type = status), and issue
 * #274 needed the same loop for the per-pool operator endpoint on cron,
 * supervisor, http and http-direct pools. Two raw HTTP servers in one project
 * is a cost nobody wanted to pay, so the loop was lifted here and both callers
 * use it: the only thing that differs between them is which paths they answer,
 * and that is a callback.
 *
 * The server is deliberately minimal -- no keep-alive, no chunked encoding, no
 * header parsing. Only the request line is read. This is a monitoring endpoint
 * scraped every 15-60 seconds, not a web server; one connection is one request,
 * one response, then close. Everything an operator endpoint could want that is
 * missing here (compression, conditional requests, authentication) is missing
 * on purpose: the endpoint is expected to be bound to a loopback or a private
 * address, which is what fpm_operator_endpoint.c defaults it to.
 */

#ifndef FPM_OPERATOR_HTTP_H
#define FPM_OPERATOR_HTTP_H 1

#include <stddef.h>

/* Growing buffer for a response body. The number of pools is naturally small
 * (the size of the configuration), so simplicity (realloc x2) matters more
 * here than avoiding a few allocations. */
struct fpm_operator_buf_s {
	char *data;
	size_t len;
	size_t cap;
};

void fpm_operator_buf_free(struct fpm_operator_buf_s *b);
void fpm_operator_buf_appendf(struct fpm_operator_buf_s *b, const char *fmt, ...);

/* What a dispatcher decides about one request. The handler fills .body and may
 * change the rest; a handler that does not recognise the path leaves .handled
 * at 0 and the server answers 404 with the caller's list of known paths. */
struct fpm_operator_reply_s {
	struct fpm_operator_buf_s body;
	const char *content_type;	/* default "text/plain; charset=utf-8" */
	int handled;
};

/* Called once per request, in the child, with the path exactly as it arrived on
 * the request line with any query string cut off, and that query string handed
 * over separately (empty, never NULL, when the request carried none).
 *
 * The path is not decoded, so a path spelled "/%73tatus" simply will not match
 * -- which is the behaviour an operator endpoint wants: a typo is a 404, not a
 * silently different answer. The query string is split off rather than left on
 * the path because it is how a status page has always been asked for a variant
 * ("?json", "?json&full"); a handler that does not care about it ignores the
 * argument, and one that does parses its own flags out of it.
 *
 * ctx is whatever was handed to fpm_operator_http_serve(). */
typedef void (*fpm_operator_http_dispatch_cb)(void *ctx, const char *path, const char *query,
	struct fpm_operator_reply_s *reply);

/* Whether a query string carries this bare flag among its '&'-separated
 * parameters -- the spelling upstream's status page uses, so "json&full" is two
 * flags and not one parameter named "json&full". query may be NULL. */
int fpm_operator_http_has_flag(const char *query, const char *flag);

/* Accept loop. Does not return: this is a child's whole life.
 *
 * known_paths is used only in the 404 body, so that a mistyped scrape URL says
 * what the right ones would have been. It may be NULL. */
void fpm_operator_http_serve(int listen_fd, fpm_operator_http_dispatch_cb cb, void *ctx,
	const char *known_paths);

#endif
