/*
 * libFuzzer harness for the static file path checks in fpm_http_static.c
 * (issue #995): fpm_http_static_decode_path() and
 * fpm_http_static_path_has_dotfile().
 *
 * Input: the raw request target path, as the client sent it, percent-encoding
 * included. The harness copies it and NUL-terminates the copy.
 *
 * Invariants, checked when decode_path() returns a path:
 *   - the length it reports is the length of the NUL-terminated string
 *     (no %00 inside the path);
 *   - the path starts with '/';
 *   - the path has no "/../" and does not end with "/..".
 * has_dotfile() must return without reading past the string it is given.
 */
#include "fpm_http_static.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Test-only shim: decode_path() reads the request's URI, and a fuzz harness has
 * no request. The harness parses the fuzz input into a URI before each call,
 * and frees it after. decode_path() does not own the URI.
 */
static struct evhttp_uri *fuzz_uri;

const struct evhttp_uri *evhttp_request_get_evhttp_uri(const struct evhttp_request *req)
{
	(void) req;
	return fuzz_uri;
}

static void fail(const char *what, const char *path)
{
	fprintf(stderr, "fuzz_static_path: %s: %s\n", what, path);
	abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char *raw = malloc(size + 1);
	size_t len = 0;
	char *path;

	if (!raw) {
		return 0;
	}
	memcpy(raw, data, size);
	raw[size] = '\0';

	fuzz_uri = evhttp_uri_parse(raw);
	path = fpm_http_static_decode_path(NULL, &len);
	if (path) {
		if (len != strlen(path)) {
			fail("reported length differs from the string length", path);
		}
		if (path[0] != '/') {
			fail("path does not start with '/'", path);
		}
		if (strstr(path, "/../")) {
			fail("path contains /../", path);
		}
		if (len >= 3 && memcmp(path + len - 3, "/..", 3) == 0) {
			fail("path ends with /..", path);
		}
		(void) fpm_http_static_path_has_dotfile(path);
		free(path);
	}
	(void) fpm_http_static_path_has_dotfile(raw);

	if (fuzz_uri) {
		evhttp_uri_free(fuzz_uri);
	}
	fuzz_uri = NULL;
	free(raw);
	return 0;
}
