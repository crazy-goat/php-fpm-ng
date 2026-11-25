/* Range and If-Range for files served by fpm_http_static_serve() (issue #648).
 *
 * Pure functions: no libevent, no filesystem. The Range header comes from the
 * public port, so the parser is kept small enough to fuzz on its own
 * (tests/fuzz/fuzz_static_range.c).
 */

#ifndef FPM_HTTP_STATIC_RANGE_H
#define FPM_HTTP_STATIC_RANGE_H 1

#include <sys/types.h>

enum fpm_http_static_range {
	FPM_HTTP_STATIC_RANGE_IGNORE = 0, /* send the whole representation with 200 */
	FPM_HTTP_STATIC_RANGE_PARTIAL = 1, /* send the bytes [*first, *last] with 206 */
	FPM_HTTP_STATIC_RANGE_UNSATISFIABLE = -1 /* send 416 */
};

/* `value` is the Range header value, `size` the length of the representation.
 * Only a single byte range is served. A header that is not a valid single
 * range, or that names several, returns IGNORE: RFC 9110 14.2 lets a server
 * ignore a Range it does not support, and a multi-range response would need
 * multipart/byteranges, which this module does not produce. */
int fpm_http_static_parse_range(const char *value, off_t size, off_t *first, off_t *last);

/* RFC 9110 13.1.5. `value` is the If-Range header. A strong entity-tag must
 * equal `etag`, a weak one never matches, and anything else must be the
 * Last-Modified date exactly as this module sent it. A mismatch means the
 * client holds another version, so the Range must be ignored. */
int fpm_http_static_if_range_matches(const char *value, const char *etag, const char *last_modified);

#endif
