/* The HTTP/1.1 client transport (issue #344): the gateway's second way to talk
 * to a target pool, next to FastCGI. `http.route[/x] = pool` may now name a
 * `pool.type = http-direct` pool, and the request continues as a plain
 * HTTP/1.1 request on the target's own listener -- the same listener the
 * pool's own clients use.
 *
 * Shape: the same four-member vtable fpm_http_internal.h defines, over the
 * same persistent raw-socket upstreams the FastCGI transport drives. That
 * machinery -- the budget, the per-target waiting queue, the pump, the
 * deferred-free discipline, the failure forensics -- is bytes- and
 * bookkeeping-identical for any stream protocol, so this file only supplies
 * what differs: who serializes the request (fpm_http_http_write_request) and
 * who parses the answer (fpm_http_http_data's status-line/header-block/body
 * framing state machine). The response is re-framed through the very same
 * fpm_http_start_reply()/fpm_http_stdout()/fpm_http_finish() the FastCGI
 * STDOUT path uses, so header handling, X-Fpmng-Queue-Wait, the access log
 * and the 502-on-no-answer rule cannot drift between the two transports.
 *
 * Why not libevent's evhttp client (evhttp_connection_base_new()): it owns
 * its own event wiring and callback structure, which would bypass exactly the
 * machinery listed above -- budgets, queues, forensics -- and reintroduce a
 * second request path this vtable exists to prevent. The response parser here
 * is deliberately as small as the protocol section it covers: status line,
 * header block, Content-Length / chunked / EOF-delimited body. The upstream
 * is always our own http-direct server (libevent >= 2.1 on both ends), so
 * there is no arbitrary third-party response to be lenient about.
 *
 * Streaming (and therefore SSE, #342): a chunked upstream response is
 * dechunked here and re-framed towards the client by evhttp, one
 * fpm_http_stdout() per dechunked piece, exactly like a FastCGI STDOUT
 * record. A stream on the target pool holds one pinned upstream connection
 * (one budget slot) for its whole life, the same cost #340 documents for a
 * FastCGI /sse target.
 */

#include "fpm_config.h"
#include "fpm_http.h"

#ifdef HAVE_FPM_HTTP

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "fpm_http_internal.h"
#include "zlog.h"

/* A response head above this bound is a protocol failure, not a big header
 * block: the upstream is our own evhttp server, whose request-side header
 * bound (FPM_HTTP_HEADERS_MAX, the same 64 KiB #117 gave the gateway) is far
 * below this. Bound the accumulator so a runaway peer cannot grow it without
 * end; reached, the connection is failed like any other protocol error. */
#define FPM_HTTP_HTTP_MAX_HEAD (256 * 1024)

/* Hop-by-hop headers, response side: never forwarded to the client.
 * Transfer-Encoding is dropped because the gateway de-chunks and evhttp
 * re-frames the body (a forwarded chunked header over re-chunked output would
 * corrupt the framing); Connection/Keep-Alive/Upgrade describe THIS
 * connection to the upstream, not the client's; Trailer belongs to framing we
 * decode; Proxy-* is the httpoxy exclusion the request side already applies
 * (issue #115) -- a header this codebase refuses in one direction does not
 * pass unrefused in the other. */
static int fpm_http_http_header_dropped(const char *key)
{
	return strcasecmp(key, "Connection") == 0
		|| strcasecmp(key, "Keep-Alive") == 0
		|| strcasecmp(key, "Transfer-Encoding") == 0
		|| strcasecmp(key, "Upgrade") == 0
		|| strcasecmp(key, "Trailer") == 0
		|| strncasecmp(key, "Proxy-", 6) == 0;
}

/* The HTTP/1.1 response parser state, one per upstream connection. Reset when
 * a response completes (fpm_http_http_complete()) -- NOT when the request is
 * written, because write_request() runs before an upstream is even chosen
 * (fpm_http_pump_target() moves the finished c->out onto whichever connection
 * is free). */
struct fpm_http_http_state_s {
	smart_str head;			/* response head bytes until the header block ends */
	size_t head_scan;		/* where the end-of-headers scan continues */
	int head_done;
	/* body framing */
	enum {
		FPM_HTTP_HTTP_BODY_CL = 0,		/* Content-Length bytes remain */
		FPM_HTTP_HTTP_BODY_CHUNK_SIZE,	/* expecting a chunk-size line */
		FPM_HTTP_HTTP_BODY_CHUNK_DATA,	/* expecting `remaining` chunk payload bytes */
		FPM_HTTP_HTTP_BODY_CHUNK_CRLF,	/* expecting the CRLF after a chunk's payload */
		FPM_HTTP_HTTP_BODY_TRAILERS,	/* reading trailer lines to the blank one */
		FPM_HTTP_HTTP_BODY_EOF			/* body ends when the connection does */
	} body;
	size_t remaining;		/* CL bytes left, or the current chunk's payload size */
	smart_str line;			/* partial chunk-size line, or a held-over '\r' of a split CRLF */
	int expect_eof;			/* the response ends at EOF; the connection is not reusable */
};

/* Is `token` one of the comma-separated values in a header like Connection or
 * Transfer-Encoding? Exact token match: a substring probe would read
 * "keep-alive" as "close" (the 'l'), which quietly turned every response into
 * a close-delimited one and dismantled connection reuse. */
static int fpm_http_http_token_in(const char *value, size_t vlen, const char *token)
{
	size_t tlen = strlen(token);
	size_t start = 0, i;

	for (i = 0; i <= vlen; i++) {
		if (i == vlen || value[i] == ',' || value[i] == ' ' || value[i] == '\t') {
			if (i - start == tlen && strncasecmp(value + start, token, tlen) == 0) {
				return 1;
			}
			start = i + 1;
		}
	}
	return 0;
}

static void fpm_http_http_state_reset(struct fpm_http_http_state_s *st)
{
	smart_str_free(&st->head);
	smart_str_free(&st->line);
	memset(st, 0, sizeof(*st));
}

static void fpm_http_http_state_free(fpm_http_upstream *up)
{
	struct fpm_http_http_state_s *st = up->http;

	if (!st) {
		return;
	}
	fpm_http_http_state_reset(st);
	free(st);
	up->http = NULL;
}

/* ------------------------------------------------------------------ request */

/* `Upgrade` is refused rather than stripped: a stripped websocket handshake
 * reaches the target pool, its 101 answer gets re-framed as an ordinary
 * response, and the client hangs mid-handshake -- an explicit 501 says what
 * is missing. WebSocket is #343's worker-executor feature; the gateway gains
 * passthrough only if a later issue decides to own it. */
static int fpm_http_http_upgrade_requested(struct evhttp_request *req)
{
	const char *upgrade = evhttp_find_header(evhttp_request_get_input_headers(req), "Upgrade");

	return upgrade != NULL && *upgrade != '\0';
}

/* X-Forwarded-For: the standard proxy append -- an incoming header (only ever
 * from an untrusted client; the gateway's trust decision is c->fwd's) is kept
 * and the address the gateway actually saw is appended, so the target pool's
 * own http.trusted_proxies can walk the chain and trust the gateway's entry. */
static void fpm_http_http_xff(smart_str *out, const char *existing, const char *effective)
{
	if (existing && *existing) {
		smart_str_appends(out, existing);
		smart_str_appends(out, ", ");
	}
	smart_str_appends(out, effective);
	smart_str_appends(out, "\r\n");
}

static int fpm_http_http_write_request(fpm_http_conn *c, int script_missing_hint)
{
	struct evhttp_request *req = c->req;
	struct evkeyvalq *in = evhttp_request_get_input_headers(req);
	struct evkeyval *header;
	size_t body_len;

	(void) script_missing_hint;	/* an http-direct target does its own docroot resolution */

	if (fpm_http_http_upgrade_requested(req)) {
		return 501;	/* Not Implemented -- see fpm_http_http_upgrade_requested() */
	}

	body_len = evbuffer_get_length(evhttp_request_get_input_buffer(req));

	smart_str_appends(&c->out, fpm_http_method_name(evhttp_request_get_command(req)));
	smart_str_appends(&c->out, " ");
	/* The request-target as received, route prefix included -- the FastCGI
	 * transport passes REQUEST_URI whole too (fpm_http_build_request()); the
	 * prefix is routing information for the GATEWAY, not something to strip
	 * from the target's view of the path. */
	smart_str_appends(&c->out, evhttp_request_get_uri(req));
	smart_str_appends(&c->out, " HTTP/1.1\r\n");

	/* Host: preserve. An HTTP/1.1 request without one is not valid, but
	 * evhttp fills a default for its own parsing, so a missing Host here is a
	 * client speaking below the version it announced -- give the target the
	 * gateway pool's name rather than leave the request headerless. */
	{
		const char *host = evhttp_find_header(in, "Host");

		smart_str_appends(&c->out, "Host: ");
		smart_str_appends(&c->out, host ? host : c->gw->pool);
		smart_str_appends(&c->out, "\r\n");
	}

	TAILQ_FOREACH(header, in, next) {
		if (fpm_http_http_header_dropped(header->key)
			|| strcasecmp(header->key, "Host") == 0
			|| strcasecmp(header->key, "Content-Length") == 0
			|| strcasecmp(header->key, "X-Forwarded-For") == 0
			|| strcasecmp(header->key, "X-Forwarded-Proto") == 0
			|| strcasecmp(header->key, "X-Forwarded-Port") == 0) {
			continue;
		}
		smart_str_appends(&c->out, header->key);
		smart_str_appends(&c->out, ": ");
		smart_str_appends(&c->out, header->value);
		smart_str_appends(&c->out, "\r\n");
	}

	/* Forwarded (fpm_http_forwarded.h): resolved once in fpm_http_request().
	 * The target pool sees the gateway as the proxy it is; its own
	 * http.trusted_proxies decides what to believe of the chain. */
	smart_str_appends(&c->out, "X-Forwarded-For: ");
	fpm_http_http_xff(&c->out, evhttp_find_header(in, "X-Forwarded-For"),
		c->fwd.remote_addr[0] ? c->fwd.remote_addr : c->peer_addr);
	smart_str_appends(&c->out, "X-Forwarded-Proto: ");
	smart_str_appends(&c->out, c->fwd.scheme[0] ? c->fwd.scheme : "http");
	smart_str_appends(&c->out, "\r\n");
	if (c->fwd.server_port[0]) {
		smart_str_appends(&c->out, "X-Forwarded-Port: ");
		smart_str_appends(&c->out, c->fwd.server_port);
		smart_str_appends(&c->out, "\r\n");
	}

	{
		char cl[32];

		snprintf(cl, sizeof(cl), "%zu", body_len);
		smart_str_appends(&c->out, "Content-Length: ");
		smart_str_appends(&c->out, cl);
		smart_str_appends(&c->out, "\r\n");
	}

	/* The gateway owns the connection; ask to keep it. The upstream answers
	 * with its own preference, which the response side reads and honours
	 * (st->expect_eof). */
	smart_str_appends(&c->out, "Connection: keep-alive\r\n\r\n");

	if (body_len) {
		const char *data = (const char *) evbuffer_pullup(evhttp_request_get_input_buffer(req), -1);

		smart_str_appendl(&c->out, data, body_len);
	}
	return 0;
}

/* ------------------------------------------------------------------ response */

static size_t fpm_http_http_value_ulong(const char *v, size_t len)
{
	size_t value = 0;

	while (len && *v >= '0' && *v <= '9') {
		value = value * 10 + (size_t) (*v - '0');
		v++;
		len--;
	}
	return value;
}

/* One header line of the response head, split in place. Returns the key start
 * and length, and the value start and length (trimmed), or 0 when the line
 * carries no colon. */
static int fpm_http_http_split_header(const char *line, size_t len,
		const char **key, size_t *key_len, const char **value, size_t *value_len)
{
	const char *colon = memchr(line, ':', len), *v;
	size_t vl;

	if (!colon) {
		return 0;
	}
	*key = line;
	*key_len = colon - line;
	v = colon + 1;
	vl = len - *key_len - 1;
	while (vl && (*v == ' ' || *v == '\t')) {
		v++;
		vl--;
	}
	while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) {
		vl--;
	}
	*value = v;
	*value_len = vl;
	return 1;
}

/* The response head is complete: turn it into the CGI header block
 * fpm_http_start_reply() parses, decide the body framing from the raw head,
 * and start the reply. Does NOT free st->head: the caller slices the body
 * bytes that rode in with the head off it first. */
static void fpm_http_http_head_done(fpm_http_upstream *up, size_t head_len)
{
	fpm_http_conn *c = up->current;
	struct fpm_http_http_state_s *st = up->http;
	const char *head = ZSTR_VAL(st->head.s), *end = head + head_len, *line;
	size_t content_length = 0;
	int has_length = 0, chunked = 0, close_seen = 0, no_body = 0, code = 200;

	/* Pass 1 over the raw head: the status code and the framing headers.
	 * These are read from the response's own headers, not from the rewritten
	 * block below -- Connection in particular describes the UPSTREAM
	 * connection, and is dropped from what the client sees. */
	line = head;
	while (line < end) {
		const char *nl = memchr(line, '\n', end - line);
		size_t len = (nl ? nl : end) - line;

		if (len && line[len - 1] == '\r') {
			len--;
		}
		if (line == head) {
			/* "HTTP/1.1 200 OK": the code is the token between the first two
			 * spaces. libevent wrote this status line, so the shape holds. */
			const char *sp = memchr(line, ' ', len);

			if (sp) {
				const char *sp2 = memchr(sp + 1, ' ', len - (size_t)(sp + 1 - line));
				size_t code_len = sp2 ? (size_t)(sp2 - sp - 1) : len - (size_t)(sp + 1 - line);

				code = (int) fpm_http_http_value_ulong(sp + 1, code_len);
			}
		} else {
			const char *key, *value;
			size_t klen, vlen;

			if (fpm_http_http_split_header(line, len, &key, &klen, &value, &vlen)) {
				if (klen == 14 && strncasecmp(key, "Content-Length", 14) == 0) {
					has_length = 1;
					content_length = fpm_http_http_value_ulong(value, vlen);
				} else if (klen == 17 && strncasecmp(key, "Transfer-Encoding", 17) == 0) {
					chunked = fpm_http_http_token_in(value, vlen, "chunked");
				} else if (klen == 10 && strncasecmp(key, "Connection", 10) == 0) {
					close_seen = fpm_http_http_token_in(value, vlen, "close");
				}
			}
		}
		line = nl ? nl + 1 : end;
	}

	if (code < 200 || code == 204 || code == 304
		|| evhttp_request_get_command(c->req) == EVHTTP_REQ_HEAD) {
		no_body = 1;
	}

	/* Pass 2: the CGI block fpm_http_start_reply() parses -- "Status:" first
	 * (the one header it treats specially), then every header we forward,
	 * verbatim, Content-Length included: the gateway honours the upstream's
	 * length with identity framing instead of stripping it and forcing evhttp
	 * to re-chunk what the upstream sent whole. */
	smart_str_free(&c->cgi_headers);
	{
		char status[48];

		snprintf(status, sizeof(status), "Status: %d\r\n", code);
		smart_str_appends(&c->cgi_headers, status);
	}
	line = head;
	while (line < end) {
		const char *nl = memchr(line, '\n', end - line);
		size_t len = (nl ? nl : end) - line;

		if (len && line[len - 1] == '\r') {
			len--;
		}
		if (line != head) {
			const char *key, *value;
			size_t klen, vlen;

			if (fpm_http_http_split_header(line, len, &key, &klen, &value, &vlen) && klen < 64) {
				char keybuf[64];

				/* key points into the head buffer without a terminator --
				 * copy first, compare second: strcasecmp on the raw pointer
				 * would read into the rest of the line and "keep-alive"
				 * would fail every hop-by-hop match. */
				memcpy(keybuf, key, klen);
				keybuf[klen] = '\0';
				if (!fpm_http_http_header_dropped(keybuf)) {
					smart_str_appends(&c->cgi_headers, keybuf);
					smart_str_appends(&c->cgi_headers, ": ");
					smart_str_appendl(&c->cgi_headers, value, vlen);
					smart_str_appends(&c->cgi_headers, "\r\n");
				}
			}
		}
		line = nl ? nl + 1 : end;
	}
	fpm_http_start_reply(c, ZSTR_LEN(c->cgi_headers.s), ZSTR_LEN(c->cgi_headers.s));

	/* Framing. */
	if (no_body) {
		st->body = FPM_HTTP_HTTP_BODY_CL;
		st->remaining = 0;
	} else if (chunked) {
		st->body = FPM_HTTP_HTTP_BODY_CHUNK_SIZE;
		st->remaining = 0;
	} else if (has_length) {
		st->body = FPM_HTTP_HTTP_BODY_CL;
		st->remaining = content_length;
	} else {
		/* RFC 9112 6.3: no framing at all means "until close". Our own
		 * servers always send one of the above, so this is the defensive
		 * branch; the response consumes the connection. */
		st->body = FPM_HTTP_HTTP_BODY_EOF;
		st->expect_eof = 1;
	}
	if (close_seen) {
		st->expect_eof = 1;
	}
}

/* One completed response. fpm_http_request_done() finishes the client reply
 * (headers are always sent by then), marks the upstream idle and pumps; a
 * response that consumed its connection is dropped instead, before that pump
 * can hand it a new request. */
static void fpm_http_http_complete(fpm_http_upstream *up)
{
	struct fpm_http_http_state_s *st = up->http;
	int expect_eof = st ? st->expect_eof : 1;

	if (st) {
		fpm_http_http_state_reset(st);
	}
	if (expect_eof) {
		fpm_http_finish(up->current, 1);
		up->current = NULL;
		fpm_http_upstream_drop(up);
		fpm_http_pump(up->gw);
		return;
	}
	fpm_http_request_done(up);
}

/* The body state machine. Returns 1 when the response completed on this
 * piece, 0 when more bytes are needed, -1 on a framing violation (the caller
 * fails the connection; errno = EPROTO for the log line). */
static int fpm_http_http_body(fpm_http_upstream *up, const char **buf, size_t *len)
{
	struct fpm_http_http_state_s *st = up->http;
	fpm_http_conn *c = up->current;

	while (*len > 0) {
		size_t take;

		switch (st->body) {
			case FPM_HTTP_HTTP_BODY_CL:
				take = MIN(st->remaining, *len);
				if (take) {
					fpm_http_stdout(c, *buf, take);
					st->remaining -= take;
					*buf += take;
					*len -= take;
				}
				if (st->remaining == 0) {
					return 1;
				}
				return 0;

			case FPM_HTTP_HTTP_BODY_CHUNK_SIZE: {
				const char *nl = memchr(*buf, '\n', *len);
				size_t line_len, i;
				const char *digits;

				if (!nl) {
					/* accumulate until the size line is complete */
					smart_str_appendl(&st->line, *buf, *len);
					if (ZSTR_LEN(st->line.s) > 16384) {
						return -1;	/* no terminator in 16 KiB of "size line": protocol garbage */
					}
					*buf += *len;
					*len = 0;
					return 0;
				}
				line_len = (size_t)(nl - *buf);
				smart_str_appendl(&st->line, *buf, line_len);
				smart_str_0(&st->line);
				/* chunk sizes are hexadecimal, with optional extensions after
				 * a ';' -- the digits run to the first non-hex character */
				digits = ZSTR_VAL(st->line.s);
				st->remaining = 0;
				for (i = 0; i < ZSTR_LEN(st->line.s); i++) {
					int ch = (unsigned char) digits[i];

					if (ch >= '0' && ch <= '9') {
						st->remaining = st->remaining * 16 + (size_t) (ch - '0');
					} else if (ch >= 'a' && ch <= 'f') {
						st->remaining = st->remaining * 16 + (size_t) (ch - 'a' + 10);
					} else if (ch >= 'A' && ch <= 'F') {
						st->remaining = st->remaining * 16 + (size_t) (ch - 'A' + 10);
					} else {
						break;
					}
				}
				smart_str_free(&st->line);
				*len -= line_len + 1;
				*buf += line_len + 1;
				st->body = st->remaining ? FPM_HTTP_HTTP_BODY_CHUNK_DATA : FPM_HTTP_HTTP_BODY_TRAILERS;
				continue;
			}

			case FPM_HTTP_HTTP_BODY_CHUNK_DATA:
				while (st->remaining && *len > 0) {
					take = MIN(st->remaining, *len);
					fpm_http_stdout(c, *buf, take);
					st->remaining -= take;
					*buf += take;
					*len -= take;
				}
				if (st->remaining == 0) {
					st->body = FPM_HTTP_HTTP_BODY_CHUNK_CRLF;
					continue;
				}
				return 0;

			case FPM_HTTP_HTTP_BODY_CHUNK_CRLF: {
				/* the CRLF after a chunk payload may straddle read pieces; a
				 * '\r' held in st->line waits for its '\n' */
				if (st->line.s && ZSTR_LEN(st->line.s)) {
					smart_str_free(&st->line);
					if (**buf != '\n') {
						return -1;
					}
					(*buf)++;
					(*len)--;
					st->body = FPM_HTTP_HTTP_BODY_CHUNK_SIZE;
					continue;
				}
				if (*len < 2) {
					if (**buf == '\r') {
						smart_str_appendl(&st->line, *buf, 1);
					} else {
						return -1;
					}
					(*buf)++;
					(*len)--;
					return 0;
				}
				if ((*buf)[0] == '\r' && (*buf)[1] == '\n') {
					st->body = FPM_HTTP_HTTP_BODY_CHUNK_SIZE;
					*buf += 2;
					*len -= 2;
					continue;
				}
				return -1;
			}

			case FPM_HTTP_HTTP_BODY_TRAILERS: {
				/* after the zero chunk: zero or more trailer lines, then the
				 * blank line. Our own servers send no trailers, so the first
				 * line is normally already the blank one -- but read the
				 * section properly rather than assume. */
				const char *nl = memchr(*buf, '\n', *len);

				if (!nl) {
					smart_str_appendl(&st->line, *buf, *len);
					if (ZSTR_LEN(st->line.s) > 16384) {
						return -1;
					}
					*buf += *len;
					*len = 0;
					return 0;
				}
				{
					size_t line_len = (size_t)(nl - *buf);

					*len -= line_len + 1;
					if (line_len == 0 || (line_len == 1 && **buf == '\r')) {
						/* the blank line: the response is complete */
						*buf += line_len + 1;
						return 1;
					}
					*buf += line_len + 1;
				}
				continue;
			}

			case FPM_HTTP_HTTP_BODY_EOF:
				fpm_http_stdout(c, *buf, *len);
				*buf += *len;
				*len = 0;
				return 0;
		}
	}
	return 0;
}

/* Feeds bytes from the upstream into the parser. The active/dead discipline
 * is fpm_http_upstream_data()'s, verbatim: completion below can reach
 * fpm_http_pump() and, through a synchronous write failure, the drop of this
 * very upstream (issue #129). */
static void fpm_http_http_data(fpm_http_upstream *up, const char *buf, size_t len)
{
	if (len > 0) {
		up->reply_seen = 1;
	}
	up->active++;
	while (len > 0) {
		struct fpm_http_http_state_s *st = up->http;

		if (!up->current || !st) {
			/* bytes for a request whose client already went away: the
			 * FastCGI transport reads and discards them record by record;
			 * here the response has nowhere to go, so the connection goes
			 * the way of any other dead one. */
			fpm_http_upstream_fail(up, 1);
			break;
		}
		if (!st->head_done) {
			size_t head_len = 0, total, tail, tail_off;

			smart_str_appendl(&st->head, buf, len);
			smart_str_0(&st->head);
			buf += len;
			len = 0;
			{
				/* Resume the scan a few bytes into the previous buffer: a
				 * "\r\n\r\n" straddling two read()s has its first byte up to
				 * three positions before the old end, and a scan that
				 * resumed exactly at head_scan would never see it. */
				const char *h = ZSTR_VAL(st->head.s);
				size_t i, from = st->head_scan > 3 ? st->head_scan - 3 : 0;

				for (i = from; i + 3 < ZSTR_LEN(st->head.s); i++) {
					if (memcmp(h + i, "\r\n\r\n", 4) == 0) {
						head_len = i + 2;	/* the head ends after the header block's final CRLF */
						break;
					}
				}
				st->head_scan = ZSTR_LEN(st->head.s);
			}
			if (!head_len) {
				if (ZSTR_LEN(st->head.s) > FPM_HTTP_HTTP_MAX_HEAD) {
					zlog(ZLOG_WARNING, "[pool %s] http: upstream '%s' sent a response head above %d bytes",
						up->gw->pool, up->t->listen_address, FPM_HTTP_HTTP_MAX_HEAD);
					fpm_http_upstream_fail(up, 0);
					break;
				}
				break;
			}
			st->head_done = 1;
			/* The tail bytes (a body fragment riding with the head) are
			 * copied out before the head buffer is freed -- feeding the
			 * parser a pointer into freed memory is exactly the bug the
			 * copy exists to avoid. */
			total = ZSTR_LEN(st->head.s);
			tail_off = head_len + 2;
			tail = total > tail_off ? total - tail_off : 0;
			fpm_http_http_head_done(up, head_len);
			if (up->dead) {
				break;
			}
			/* A response whose body is already finished by its framing --
			 * Content-Length: 0, 204/304, a HEAD request -- has nothing left
			 * to wait for: no further byte will ever arrive on a keep-alive
			 * connection, and without completing here the request would pin
			 * its budget slot for ever. complete() honours expect_eof (a
			 * bodiless response that also said Connection: close drops the
			 * upstream instead of reusing it). */
			if (!tail && st->body == FPM_HTTP_HTTP_BODY_CL && st->remaining == 0) {
				fpm_http_http_complete(up);
				if (up->dead) {
					break;
				}
				continue;
			}
			if (tail) {
				char *tailcopy = malloc(tail);
				const char *p;

				if (!tailcopy) {
					errno = ENOMEM;
					fpm_http_upstream_fail(up, 0);
					break;
				}
				memcpy(tailcopy, ZSTR_VAL(st->head.s) + tail_off, tail);
				smart_str_free(&st->head);
				p = tailcopy;
				{
					int rc = fpm_http_http_body(up, &p, &tail);

					free(tailcopy);
					if (up->dead) {
						break;
					}
					if (rc < 0) {
						errno = EPROTO;
						fpm_http_upstream_fail(up, 0);
						break;
					}
					if (rc > 0) {
						fpm_http_http_complete(up);
						if (up->dead) {
							break;
						}
					}
				}
			} else {
				smart_str_free(&st->head);
			}
			continue;
		}
		{
			int rc = fpm_http_http_body(up, &buf, &len);

			if (up->dead) {
				break;
			}
			if (rc < 0) {
				errno = EPROTO;
				fpm_http_upstream_fail(up, 0);
				break;
			}
			if (rc > 0) {
				fpm_http_http_complete(up);
				if (up->dead) {
					break;
				}
			}
		}
	}
	up->active--;
	if (up->dead && !up->active) {
		/* Only the free() was deferred; the connection slot went back to the
		 * pool at detach time and whoever dropped this upstream has already
		 * pumped, so there is nothing to dispatch from here. Issue #129. */
		fpm_http_upstream_free(up);
	}
}

/* The read callback bound as this transport's ops->on_readable. Same shape as
 * fpm_http.c's FastCGI readcb -- same 16 KiB reads, same EINTR/EAGAIN dance --
 * feeding the HTTP parser instead of the record parser. */
static void fpm_http_http_readcb(evutil_socket_t fd, short what, void *arg)
{
	fpm_http_upstream *up = arg;
	char buf[16 * 1024];
	ssize_t n;

	if (what & EV_TIMEOUT) {
		/* idle for a while: give the target's worker back to the pool */
		if (!up->busy) {
			fpm_http_upstream_drop(up);
		}
		return;
	}
	do {
		n = read(fd, buf, sizeof(buf));
	} while (n < 0 && errno == EINTR);

	if (n > 0) {
		fpm_http_http_data(up, buf, (size_t) n);
	} else if (n == 0) {
		struct fpm_http_http_state_s *st = up->http;

		/* An idle upstream closing is normal keep-alive economics (the
		 * target's own idle timeout, pm.max_requests recycling). A BUSY one
		 * closing before the response was complete is a lost reply mid-body
		 * -- worth its own line, since the clean-EOF path below would
		 * otherwise end the client's truncated stream in silence. */
		if (up->busy && up->current && st && st->head_done
			&& !st->expect_eof && st->body != FPM_HTTP_HTTP_BODY_EOF) {
			zlog(ZLOG_WARNING, "[pool %s] http: upstream '%s' closed before the response was complete",
				up->gw->pool, up->t->listen_address);
		}
		fpm_http_upstream_fail(up, 1);
	} else if (!fpm_http_would_block(errno)) {
		fpm_http_upstream_fail(up, 0);
	}
}

/* connect(): the generic one in fpm_http.c (budget, socket, events), plus
 * this transport's parser state. */
static fpm_http_upstream *fpm_http_http_connect(struct fpm_http_target_s *t)
{
	fpm_http_upstream *up = fpm_http_transport_connect(t);

	if (!up) {
		return NULL;
	}
	up->http = calloc(1, sizeof(struct fpm_http_http_state_s));
	if (!up->http) {
		fpm_http_upstream_drop(up);
		return NULL;
	}
	return up;
}

static void fpm_http_http_drop(fpm_http_upstream *up)
{
	fpm_http_http_state_free(up);
	fpm_http_upstream_drop(up);
}

/* Bound as t->ops at config time (fpm_http_target_init(), issue #344). */
static const struct fpm_http_transport_s fpm_http_target_http_ops_s = {
	fpm_http_http_connect,
	fpm_http_http_write_request,
	fpm_http_http_readcb,
	fpm_http_http_drop
};

const struct fpm_http_transport_s *fpm_http_target_http_ops(void)
{
	return &fpm_http_target_http_ops_s;
}

#endif /* HAVE_FPM_HTTP */
