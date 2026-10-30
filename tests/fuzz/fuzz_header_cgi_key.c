/*
 * libFuzzer harness for fpm_http_header_cgi_key() (issue #995): maps a request
 * header name to its CGI variable name, HTTP_<NAME> with '-' turned into '_'.
 *
 * Input layout:
 *   byte 0      bit 0 set = content_type_skip
 *   byte 1      sets the output buffer size: 1 + 5 * byte 1, so 1..1276 bytes
 *   bytes 2..   the header name, NUL-terminated for the call
 *
 * The output buffer is allocated with exactly out_size bytes, so
 * AddressSanitizer reports any write past it.
 *
 * Oracle: the mapping written out again from the specification. Bytes other
 * than a-z and '-' pass through unchanged. The harness checks the return value,
 * the output on success, and that the output buffer is untouched on refusal.
 */
#include "fpm_http_header_cgi.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int ref_key(const char *name, int content_type_skip, size_t out_size, char *want)
{
	size_t len = strlen(name);
	size_t i;

	if (len > FPM_HTTP_HEADER_NAME_MAX || out_size < len + sizeof("HTTP_")) {
		return -1;
	}
	if (strcasecmp(name, "Content-Length") == 0 || strcasecmp(name, "Proxy") == 0) {
		return 1;
	}
	if (memchr(name, '_', len) != NULL) {
		return 1;
	}
	if (strcasecmp(name, "Content-Type") == 0) {
		if (content_type_skip) {
			return 1;
		}
		strcpy(want, "CONTENT_TYPE");
		return 0;
	}
	memcpy(want, "HTTP_", sizeof("HTTP_") - 1);
	for (i = 0; i < len; i++) {
		char ch = name[i];

		if (ch >= 'a' && ch <= 'z') {
			ch = (char) (ch - ('a' - 'A'));
		} else if (ch == '-') {
			ch = '_';
		}
		want[sizeof("HTTP_") - 1 + i] = ch;
	}
	want[sizeof("HTTP_") - 1 + len] = '\0';
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char want[FPM_HTTP_HEADER_CGI_KEY_LEN];
	size_t name_len, out_size, i;
	char *name, *out;
	int got, ref;

	if (size < 2) {
		return 0;
	}
	out_size = 1 + 5 * (size_t) data[1];
	name_len = size - 2;
	name = malloc(name_len + 1);
	out = malloc(out_size);
	if (!name || !out) {
		free(name);
		free(out);
		return 0;
	}
	memcpy(name, data + 2, name_len);
	name[name_len] = '\0';
	memset(out, 'Z', out_size);

	got = fpm_http_header_cgi_key(name, data[0] & 1, out, out_size);
	ref = ref_key(name, data[0] & 1, out_size, want);
	if (got != ref) {
		fprintf(stderr, "fuzz_header_cgi_key: returns %d, reference %d\n", got, ref);
		abort();
	}
	if (got != 0) {
		for (i = 0; i < out_size; i++) {
			if (out[i] != 'Z') {
				fprintf(stderr, "fuzz_header_cgi_key: output written on return %d\n", got);
				abort();
			}
		}
	} else if (strcmp(out, want) != 0) {
		fprintf(stderr, "fuzz_header_cgi_key: output \"%s\", reference \"%s\"\n", out, want);
		abort();
	}
	free(name);
	free(out);
	return 0;
}
