/*
 * libFuzzer harness for the FastCGI record parser in fpm_http.c (issue #995).
 *
 * Input layout:
 *   byte 0      seed of the read-split schedule
 *   bytes 1..   the record stream a pool would send
 *
 * The stream runs twice: once in one read, and once cut into reads whose
 * sizes come from the seed. A correct record parser does not depend on where
 * the reads split, so both runs must produce the same CGI bytes and finish the
 * request the same number of times. A mismatch aborts, which libFuzzer reports
 * as a crash with the reproducer.
 *
 * fpm_http.c is #included so that its static record parser is visible. Its
 * request_done(), pump(), tick_arm() and upstream_free() run for real; the
 * gateway and the upstream have no targets, no siblings and no idle timer, so
 * they do nothing that needs an event loop.
 */
#include "fpm_http.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct run_result {
	unsigned char *out;
	size_t out_len;
	unsigned finished;
};

/* Strong overrides of the fpm_http_fcgi.c functions: capture, do not send. */
static struct run_result *current_run;

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

static uint32_t xorshift(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static void run_stream(const uint8_t *data, size_t len, uint32_t split_seed, int split, struct run_result *r)
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
	gw->idle_ms = 0;
	t->gw = gw;
	t->pool = pool;
	t->listen_address = listen;
	up->gw = gw;
	up->t = t;
	up->current = c;
	up->busy = 1;

	current_run = r;
	if (!split) {
		fpm_http_upstream_data(up, (const char *) data, len);
	} else {
		while (off < len) {
			size_t take = 1 + xorshift(&rng) % 97;

			if (take > len - off) {
				take = len - off;
			}
			fpm_http_upstream_data(up, (const char *) data + off, take);
			off += take;
		}
	}
	/* Nothing in this harness frees the upstream, so it is still ours. */
	free(up);
	free(t);
	free(gw);
	free(c);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct run_result whole = {0}, split = {0};
	uint32_t split_seed;

	if (size < 2) {
		return 0;
	}
	split_seed = (uint32_t) data[0] * 2654435761u | 1u;
	run_stream(data + 1, size - 1, 0, 0, &whole);
	run_stream(data + 1, size - 1, split_seed, 1, &split);
	if (whole.out_len != split.out_len || (whole.out_len && memcmp(whole.out, split.out, whole.out_len) != 0)) {
		fprintf(stderr, "fuzz_fcgi_upstream: CGI bytes depend on the read split (%zu vs %zu)\n",
			whole.out_len, split.out_len);
		abort();
	}
	if (whole.finished != split.finished) {
		fprintf(stderr, "fuzz_fcgi_upstream: finish count depends on the read split (%u vs %u)\n",
			whole.finished, split.finished);
		abort();
	}
	free(whole.out);
	free(split.out);
	return 0;
}
