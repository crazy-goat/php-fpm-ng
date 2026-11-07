/*
 * libFuzzer harness for fpm_http_proxy_parse() (issue #651): the PROXY protocol
 * v1 and v2 header parser of the gateway.
 *
 * Input: the bytes a connection has delivered so far. Every call parses the
 * whole input, as the reader does on each loop pass. The input is copied to a
 * heap buffer of exactly its size, so AddressSanitizer reports any read past
 * it. The input is never shortened: a cut input could hide an over-read.
 *
 * Invariants, checked on every call:
 *   - NEED_MORE is returned only for an input shorter than FPM_HTTP_PROXY_MAX,
 *     so the reader's buffer is never overrun;
 *   - DONE reports a length from 1 to the input length;
 *   - a prefix of a complete header is NEED_MORE, never DONE and never BAD:
 *     the parser must not decide early, and must not reject bytes that are
 *     still a valid start;
 *   - an address, when has_source is set, is valid text and NUL-terminated.
 */
#include "fpm_http_proxy_protocol.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *what)
{
	fprintf(stderr, "fuzz_proxy_protocol: %s\n", what);
	abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct fpm_http_proxy_header_s hdr;
	enum fpm_http_proxy_status st;
	unsigned char *buf;
	size_t k;

	buf = malloc(size ? size : 1);
	if (!buf) {
		return 0;
	}
	if (size) {
		memcpy(buf, data, size);
	}
	memset(&hdr, 0, sizeof(hdr));
	st = fpm_http_proxy_parse(buf, size, &hdr);
	if (st == FPM_HTTP_PROXY_NEED_MORE && size >= FPM_HTTP_PROXY_MAX) {
		fail("NEED_MORE at or above the buffer bound");
	}
	if (st == FPM_HTTP_PROXY_DONE) {
		if (hdr.length == 0 || hdr.length > size) {
			fail("DONE with a length outside the input");
		}
		if (hdr.has_source && !memchr(hdr.addr, '\0', sizeof(hdr.addr))) {
			fail("address not NUL-terminated");
		}
		for (k = 0; k < hdr.length; k++) {
			struct fpm_http_proxy_header_s tmp;
			enum fpm_http_proxy_status pre = fpm_http_proxy_parse(buf, k, &tmp);

			if (pre != FPM_HTTP_PROXY_NEED_MORE) {
				fail("a prefix of a complete header is not NEED_MORE");
			}
		}
	}
	free(buf);
	return 0;
}
