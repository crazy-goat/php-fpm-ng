/*
 * libFuzzer harness for fpm_http_decoded_path_ok() in fpm_http_fcgi.c
 * (issue #997): the check that keeps a request path from becoming a file name
 * outside the document root.
 *
 * Input: the raw request target path, as the client sent it, percent-encoding
 * included. The harness parses it with EVHTTP_URI_NONCONFORMANT, as libevent
 * does (http.c), and decodes the path as fpm_http_build_request() does.
 *
 * Invariant, checked when the predicate accepts the path:
 *   - the path starts with '/';
 *   - the length it reports is the length of the NUL-terminated string
 *     (no %00 inside the path);
 *   - the path has no "/../" and does not end with "/..".
 */
#include "fpm_http_fcgi.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *what, const char *path)
{
	fprintf(stderr, "fuzz_decoded_path: %s: %s\n", what, path);
	abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char *raw = malloc(size + 1);
	struct evhttp_uri *uri;
	const char *target;
	char *decoded;
	size_t decoded_len = 0;

	if (!raw) {
		return 0;
	}
	memcpy(raw, data, size);
	raw[size] = '\0';

	uri = evhttp_uri_parse_with_flags(raw, EVHTTP_URI_NONCONFORMANT);
	target = uri ? evhttp_uri_get_path(uri) : NULL;
	if (target && *target) {
		decoded = evhttp_uridecode(target, 0, &decoded_len);
		if (decoded) {
			if (fpm_http_decoded_path_ok(decoded, decoded_len)) {
				if (decoded[0] != '/') {
					fail("accepted a path that does not start with '/'", decoded);
				}
				if (decoded_len != strlen(decoded)) {
					fail("reported length differs from the string length", decoded);
				}
				if (strstr(decoded, "/../")) {
					fail("accepted a path with /../", decoded);
				}
				if (decoded_len >= 3 && memcmp(decoded + decoded_len - 3, "/..", 3) == 0) {
					fail("accepted a path that ends with /..", decoded);
				}
			}
			free(decoded);
		}
	}

	if (uri) {
		evhttp_uri_free(uri);
	}
	free(raw);
	return 0;
}
