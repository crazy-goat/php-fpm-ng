/*
 * libFuzzer harness for fcgi_get_params() in third_party/php-src/main/fastcgi.c
 * (issue #995): the FastCGI PARAMS name/value stream that a web server sends.
 *
 * Input: the PARAMS record body, bytes exactly as received.
 *
 * fastcgi.c is #included from the pristine php-src copy in the build tree, so
 * fcgi_get_params() and the hash helpers are the production code. Each input
 * builds a fresh request environment and destroys it again.
 *
 * Oracle: a reference walk of the name/value length format with 64-bit
 * arithmetic. Production must never accept a stream the reference rejects.
 * The check is one-sided: production may refuse a stream the reference accepts.
 */
#include "fastcgi.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_len(const unsigned char *p, size_t n, size_t *i, uint64_t *v)
{
	if (*i >= n) {
		return 0;
	}
	if (!(p[*i] & 0x80)) {
		*v = p[(*i)++];
		return 1;
	}
	if (n - *i < 4) {
		return 0;
	}
	*v = ((uint64_t) (p[*i] & 0x7f) << 24) | ((uint64_t) p[*i + 1] << 16) |
		((uint64_t) p[*i + 2] << 8) | p[*i + 3];
	*i += 4;
	return 1;
}

static int reference_wellformed(const unsigned char *p, size_t n)
{
	size_t i = 0;

	while (i < n) {
		uint64_t nl, vl;

		if (!read_len(p, n, &i, &nl) || !read_len(p, n, &i, &vl)) {
			return 0;
		}
		if (nl + vl > n - i) {
			return 0;
		}
		i += (size_t) (nl + vl);
	}
	return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	fcgi_request req;
	unsigned char *buf;
	int rc, want;

	/* Slack past the record mirrors the FCGI_MAX_LENGTH+8 buffer in fcgi_read_request(). */
	buf = calloc(size + 8, 1);
	if (!buf) {
		return 0;
	}
	memcpy(buf, data, size);
	memset(&req, 0, sizeof(req));
	fcgi_hash_init(&req.env);

	rc = fcgi_get_params(&req, buf, buf + size);
	want = reference_wellformed(data, size);
	if (rc && !want) {
		fprintf(stderr, "fuzz_fcgi_params: accepted a stream the reference rejects (%zu bytes)\n", size);
		abort();
	}

	fcgi_hash_destroy(&req.env);
	free(buf);
	return 0;
}
