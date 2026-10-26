/*
 * libFuzzer harness for fpm_http_log_escape() (issue #995): makes a value safe
 * for one log line by writing printable bytes as they are and every other byte
 * as a \xNN escape.
 *
 * Input layout:
 *   byte 0      sets the output buffer size: 1 + byte 0 % 200
 *   byte 1      sets max, the number of input bytes the function may read
 *   bytes 2..   the input value, copied and NUL-terminated for the call
 *
 * Invariants, checked on every call:
 *   - the output is NUL-terminated and shorter than the buffer;
 *   - the output has only the bytes 0x21..0x7e except backslash, or complete
 *     \xNN escapes with lowercase hex digits;
 *   - decoding the output gives exactly the first k input bytes, where
 *     k <= min(max, strlen(input));
 *   - the output stops early only when the next byte would not fit.
 *
 * out_size is at least 1.
 */
#include "fpm_http_direct_request.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_val(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

static void fail(const char *what)
{
	fprintf(stderr, "fuzz_log_escape: %s\n", what);
	abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	size_t out_size, max, in_len, lim, o = 0, k = 0;
	char *in, *out, *dec;
	const char *ret;

	if (size < 2) {
		return 0;
	}
	out_size = 1 + data[0] % 200;
	max = data[1];
	in_len = size - 2;
	in = malloc(in_len + 1);
	out = malloc(out_size);
	dec = malloc(in_len + 1);
	if (!in || !out || !dec) {
		abort();
	}
	memcpy(in, data + 2, in_len);
	in[in_len] = '\0';
	in_len = strlen(in);
	lim = max < in_len ? max : in_len;

	ret = fpm_http_log_escape(in, max, out, out_size);
	if (ret != out) {
		fail("return value is not the output buffer");
	}
	o = strlen(out);
	if (o >= out_size) {
		fail("output is not shorter than the buffer");
	}

	for (size_t i = 0; i < o;) {
		unsigned char c = (unsigned char) out[i];

		if (c == '\\') {
			int hi, lo;

			if (i + 3 >= o || out[i + 1] != 'x') {
				fail("backslash is not a complete \\xNN escape");
			}
			hi = hex_val(out[i + 2]);
			lo = hex_val(out[i + 3]);
			if (hi < 0 || lo < 0) {
				fail("escape has a non-hex digit or uppercase hex");
			}
			dec[k++] = (char) (hi * 16 + lo);
			i += 4;
		} else {
			if (c < 0x21 || c > 0x7e) {
				fail("output has a byte outside the printable range");
			}
			dec[k++] = (char) c;
			i++;
		}
	}

	if (k > lim) {
		fail("output decodes to more bytes than max or the input allows");
	}
	if (memcmp(dec, in, k) != 0) {
		fail("output does not decode to the input prefix");
	}
	if (k < lim) {
		/* The loop stopped early: the next byte must not have fit. */
		unsigned char next = (unsigned char) in[k];
		int printable = next >= 0x21 && next <= 0x7e && next != '\\';
		size_t need = printable ? 1 : 4;

		if (o + need < out_size) {
			fail("output stopped early although the next byte fits");
		}
	}
	free(in);
	free(out);
	free(dec);
	return 0;
}
