/*
 * libFuzzer harness for the HTTP/1.1 response parser in fpm_http_client.c
 * (issue #995): status line, headers, Content-Length, chunked framing,
 * trailers, interim 1xx responses and the HEAD special case.
 *
 * Input layout:
 *   byte 0      bit 0 set = the request was HEAD (no body is expected)
 *   byte 1      seed of the read-split schedule
 *   bytes 2..   the bytes an upstream would send back
 *
 * The response runs twice: once in one read, and once cut into reads whose
 * sizes come from the seed. The HTTP parser keeps state across reads, so both
 * runs must produce the same body bytes and the same number of finished
 * replies. A mismatch aborts, which libFuzzer reports as a crash. Once the
 * parser fails or drops the upstream, the read loop stops, as the event loop does.
 *
 * fpm_http_client.c is #included for its static parser. Everything the parser
 * calls back into is defined below as a strong function: the captures are
 * simple counters, and no client, socket or event loop is involved.
 */
#include "fpm_http_client.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sapi/embed/php_embed.h"

/*
 * Test-only shim: the parser asks the request for its method, and a fuzz
 * harness has no request. The fuzz input chooses the method for each run.
 */
static enum evhttp_cmd_type fuzz_method = EVHTTP_REQ_GET;

enum evhttp_cmd_type evhttp_request_get_command(const struct evhttp_request *req)
{
	(void) req;
	return fuzz_method;
}

struct run_result {
	unsigned char *out;
	size_t out_len;
	unsigned finished;
	unsigned replies;
	unsigned fails;
	unsigned drops;
	unsigned reqs_done;
	unsigned freed;
};

static struct run_result *current_run;
static char dummy_request;

/* Strong overrides of the fpm_http_fcgi.c and fpm_http.c functions. */
void fpm_http_start_reply(fpm_http_conn *c, size_t cgi_len, size_t cgi_total)
{
	(void) c;
	(void) cgi_len;
	(void) cgi_total;
	current_run->replies++;
}

void fpm_http_stdout(fpm_http_conn *c, const char *buf, size_t len)
{
	struct run_result *r = current_run;

	(void) c;
	r->out = realloc(r->out, r->out_len + len);
	if (!r->out) {
		abort();
	}
	memcpy(r->out + r->out_len, buf, len);
	r->out_len += len;
}

void fpm_http_finish(fpm_http_conn *c, int truncated)
{
	(void) c;
	(void) truncated;
	current_run->finished++;
}

/* Mirrors fpm_http_request_done() in fpm_http.c, without the event side. */
void fpm_http_request_done(fpm_http_upstream *up)
{
	if (up->current) {
		fpm_http_finish(up->current, 1);
		up->current = NULL;
	}
	up->busy = 0;
	up->req_written = up->reply_seen = 0;
	current_run->reqs_done++;
}

/* The parser may fail the upstream. Production then closes it, so no more
 * bytes arrive: the harness marks it dead, and the read loop stops. */
void fpm_http_upstream_fail(fpm_http_upstream *up, int clean_eof)
{
	(void) clean_eof;
	up->dead = 1;
	current_run->fails++;
}

/* Mirrors the real drop, which only sets `dead` while a caller is active. */
void fpm_http_upstream_drop(fpm_http_upstream *up)
{
	if (!up->active) {
		fprintf(stderr, "fuzz_http_client: drop with no active caller\n");
		abort();
	}
	up->dead = 1;
	current_run->drops++;
}

/* The harness owns the upstream and frees it after the run. */
void fpm_http_upstream_free(fpm_http_upstream *up)
{
	(void) up;
	current_run->freed++;
}

static uint32_t xorshift(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static void run_response(const uint8_t *data, size_t len, uint32_t split_seed, int split, int head,
	struct run_result *r)
{
	static char pool[] = "fuzz";
	static char listen[] = "fuzz";
	struct fpm_http_gateway_s *gw = calloc(1, sizeof(*gw));
	struct fpm_http_target_s *t = calloc(1, sizeof(*t));
	fpm_http_upstream *up = calloc(1, sizeof(*up));
	fpm_http_conn *c = calloc(1, sizeof(*c));
	size_t off = 0;
	uint32_t rng = split_seed;

	if (!gw || !t || !up || !c) {
		abort();
	}
	gw->pool = pool;
	t->gw = gw;
	t->pool = pool;
	t->listen_address = listen;
	t->ops = fpm_http_target_http_ops();
	c->gw = gw;
	c->target = t;
	c->req = (struct evhttp_request *) &dummy_request;
	up->gw = gw;
	up->t = t;
	up->current = c;
	up->busy = 1;
	up->http = calloc(1, sizeof(struct fpm_http_http_state_s));
	if (!up->http) {
		abort();
	}

	fuzz_method = head ? EVHTTP_REQ_HEAD : EVHTTP_REQ_GET;
	current_run = r;
	if (!split) {
		fpm_http_http_data(up, (const char *) data, len);
	} else {
		while (off < len) {
			size_t take = 1 + xorshift(&rng) % 61;

			if (take > len - off) {
				take = len - off;
			}
			fpm_http_http_data(up, (const char *) data + off, take);
			off += take;
			if (up->dead) {
				break;
			}
		}
	}
	fpm_http_http_state_free(up);
	smart_str_free(&c->cgi_headers);
	free(up);
	free(t);
	free(gw);
	free(c);
}

/* Shuts the embedded runtime down at exit. Without it, LeakSanitizer reports
 * the runtime's start-up allocations as leaks, and the run fails at exit. */
static void fuzz_php_shutdown(void)
{
	php_embed_shutdown();
}

/* Starts the embedded PHP runtime once. smart_str and emalloc() need it. */
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void) argc;
	(void) argv;
	if (php_embed_init(0, NULL) != SUCCESS) {
		fprintf(stderr, "fuzz_http_client: php_embed_init failed\n");
		abort();
	}
	if (atexit(fuzz_php_shutdown) != 0) {
		abort();
	}
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct run_result whole = {0}, split = {0};
	int head;
	uint32_t split_seed;

	if (size < 3) {
		return 0;
	}
	head = data[0] & 1;
	split_seed = (uint32_t) data[1] * 2654435761u | 1u;
	run_response(data + 2, size - 2, 0, 0, head, &whole);
	run_response(data + 2, size - 2, split_seed, 1, head, &split);
	if (whole.out_len != split.out_len || (whole.out_len && memcmp(whole.out, split.out, whole.out_len) != 0)) {
		fprintf(stderr, "fuzz_http_client: body bytes depend on the read split (%zu vs %zu)\n",
			whole.out_len, split.out_len);
		abort();
	}
	if (whole.finished != split.finished) {
		fprintf(stderr, "fuzz_http_client: finish count depends on the read split (%u vs %u)\n",
			whole.finished, split.finished);
		abort();
	}
	free(whole.out);
	free(split.out);
	return 0;
}
