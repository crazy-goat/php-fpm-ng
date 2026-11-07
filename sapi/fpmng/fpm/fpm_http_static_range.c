/* Range and If-Range for static files. See fpm_http_static_range.h. */

#include "fpm_config.h"

#include "fpm_http_static_range.h"

#include <limits.h>
#include <string.h>
#include <strings.h>

/* Reads a run of decimal digits at *pp and advances *pp past it. A value that
 * would not fit is clamped to LLONG_MAX rather than wrapped: a clamped first
 * position is past the end of any file, and a clamped suffix is longer than
 * any file, so the clamp gives the answer the unclamped number would have. */
static size_t fpm_http_static_range_digits(const char **pp, long long *out)
{
	const char *p = *pp;
	size_t n = 0;

	*out = 0;
	for (; *p >= '0' && *p <= '9'; p++, n++) {
		int d = *p - '0';

		if (*out > (LLONG_MAX - d) / 10) {
			*out = LLONG_MAX;
		} else {
			*out = *out * 10 + d;
		}
	}
	*pp = p;

	return n;
}

int fpm_http_static_parse_range(const char *value, off_t size, off_t *first, off_t *last)
{
	const char *p;
	long long a = 0, b = 0, len = (long long) size;
	size_t na, nb = 0;
	int suffix, has_b;

	if (!value || strncasecmp(value, "bytes=", 6) != 0) {
		return FPM_HTTP_STATIC_RANGE_IGNORE;
	}
	p = value + 6;
	suffix = *p == '-';
	if (suffix) {
		p++;
	}
	na = fpm_http_static_range_digits(&p, &a);
	if (suffix) {
		/* "-n" is the last n bytes. "-" alone and "-x" are not a range. */
		if (!na || *p) {
			return FPM_HTTP_STATIC_RANGE_IGNORE;
		}
		/* suffix-length 0 selects nothing, so it is unsatisfiable (RFC 9110
		 * 14.1.1). So is any range on an empty file: there is no byte to
		 * select, and a 0-0 answer would be a lie. */
		if (a == 0 || len == 0) {
			return FPM_HTTP_STATIC_RANGE_UNSATISFIABLE;
		}
		if (a > len) {
			a = len;
		}
		*first = (off_t) (len - a);
		*last = (off_t) (len - 1);
		return FPM_HTTP_STATIC_RANGE_PARTIAL;
	}
	/* "a-b" or "a-". Anything else, including a comma, is not a single range
	 * this module serves, and the header is ignored. */
	if (!na || *p != '-') {
		return FPM_HTTP_STATIC_RANGE_IGNORE;
	}
	p++;
	nb = fpm_http_static_range_digits(&p, &b);
	has_b = nb > 0;
	if (*p) {
		return FPM_HTTP_STATIC_RANGE_IGNORE;
	}
	/* last-pos below first-pos makes the byte-range-spec invalid (RFC 9110
	 * 14.1.1), and an invalid Range is ignored, not answered with 416. */
	if (has_b && b < a) {
		return FPM_HTTP_STATIC_RANGE_IGNORE;
	}
	if (a >= len) {
		return FPM_HTTP_STATIC_RANGE_UNSATISFIABLE;
	}
	*first = (off_t) a;
	*last = (off_t) (has_b && b < len ? b : len - 1);

	return FPM_HTTP_STATIC_RANGE_PARTIAL;
}

int fpm_http_static_if_range_matches(const char *value, const char *etag, const char *last_modified)
{
	if (value[0] == '"') {
		return strcmp(value, etag) == 0;
	}
	if (value[0] == 'W' && value[1] == '/') {
		return 0;
	}

	return last_modified[0] && strcmp(value, last_modified) == 0;
}
