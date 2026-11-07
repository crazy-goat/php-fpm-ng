/*
 * libFuzzer harness for fpm_http_host_parse() in fpm_http_host.c (issue
 * #1069): the parser that turns the untrusted Host header (or the authority of
 * an absolute-form target) into the name a host-bound http.route[] entry is
 * matched on (issue #658).
 *
 * Input: the raw Host value, as the client sent it, NUL bytes included. The
 * name is parsed into the FPM_HTTP_AUTHORITY_MAX buffer the request path uses
 * (fpm_http.c).
 *
 * Invariant, checked when the parser accepts the value -- an oracle that does
 * not repeat the grammar it fuzzes:
 *   - the name contains only lower-case letters, digits, '.' and '-';
 *   - the name has no empty label: no doubled dot, no leading or trailing dot;
 *   - the name is never longer than the input it was parsed from.
 */
#include "fpm_http_host.c"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void fail(const char *what, const char *host)
{
	fprintf(stderr, "fuzz_http_host: %s: %s\n", what, host);
	abort();
}

static int is_name_byte(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char out[FPM_HTTP_AUTHORITY_MAX];

	if (fpm_http_host_parse((const char *) data, size, out, sizeof(out))) {
		size_t len = strlen(out), i;

		if (len > size) {
			fail("accepted a name longer than its input", out);
		}
		for (i = 0; i < len; i++) {
			if (!is_name_byte((unsigned char) out[i])) {
				fail("accepted a name with a byte outside [a-z0-9.-]", out);
			}
		}
		if (out[0] == '.' || out[len - 1] == '.' || strstr(out, "..")) {
			fail("accepted a name with an empty label", out);
		}
	}
	return 0;
}
