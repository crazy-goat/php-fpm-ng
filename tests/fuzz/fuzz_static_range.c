/*
 * libFuzzer harness for the Range and If-Range parsers in
 * fpm_http_static_range.c (issue #648). The Range header comes from the public
 * port, so the parser reads bytes an attacker chooses.
 *
 * Input layout:
 *   byte 0      the size of the representation: the value, 0..255 bytes; with
 *               bit 0 of byte 1 set (when there is one), 2^40 bytes instead,
 *               so the clamp at LLONG_MAX and the offsets past 32 bits run
 *   bytes 1..   the Range header value, NUL-terminated for the call
 *
 * Invariants, checked when parse_range() returns PARTIAL:
 *   - 0 <= first <= last < size, so the range lies inside the representation;
 *   - a range is never returned for an empty representation.
 * The If-Range matcher and the Accept-Encoding parser
 * fpm_http_static_accepts() (fpm_http_static_negotiate.c, also public-port
 * bytes) are called on the same bytes and must return without reading past
 * the string they are given.
 */
#include "fpm_http_static_range.c"
#include "fpm_http_static_negotiate.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *what, const char *value)
{
	fprintf(stderr, "fuzz_static_range: %s: %s\n", what, value);
	abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char *value;
	off_t len, first = -1, last = -1;
	int result;

	if (size < 1) {
		return 0;
	}
	len = data[0];
	if (size > 1 && (data[1] & 1)) {
		len = (off_t) 1 << 40;
	}
	value = malloc(size);
	if (!value) {
		return 0;
	}
	memcpy(value, data + 1, size - 1);
	value[size - 1] = '\0';

	result = fpm_http_static_parse_range(value, len, &first, &last);
	if (result == FPM_HTTP_STATIC_RANGE_PARTIAL) {
		if (len == 0) {
			fail("a range was returned for an empty representation", value);
		}
		if (first < 0 || last < first || last >= len) {
			fail("range outside the representation", value);
		}
	}
	(void) fpm_http_static_if_range_matches(value, "\"1-2\"", "Mon, 01 Jan 2024 00:00:00 GMT");
	(void) fpm_http_static_accepts(value, "br");
	(void) fpm_http_static_accepts(value, "gzip");

	free(value);
	return 0;
}
