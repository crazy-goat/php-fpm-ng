/* fpm-ng: HTTP gateway access log (http.access_log), Combined Log Format,
 * not configurable — see docs/NOTES.md for why (simplicity is valuable here).
 *
 * Every http.gateways gateway process has its own descriptor for THE SAME file,
 * opened with O_APPEND. One write() per line on a regular O_APPEND file is
 * atomic relative to other writers (POSIX), so lines from gateway processes do
 * not interleave despite having no shared lock. Writes are synchronous (as in
 * nginx/Apache) — a local disk/page cache makes this cheap, so an asynchronous
 * buffering layer is not worth the complexity.
 */

#ifndef FPM_HTTP_ACCESS_LOG_H
#define FPM_HTTP_ACCESS_LOG_H 1

#include <stddef.h>

struct fpm_http_access_log_s;

/* path == NULL or "" -> logging disabled, returns NULL (a valid "disabled"
 * handle for fpm_http_access_log_write()). Open errors are logged by zlog and
 * treated like "disabled" — a missing log must not prevent gateway startup. */
struct fpm_http_access_log_s *fpm_http_access_log_open(const char *pool, const char *path);

void fpm_http_access_log_close(struct fpm_http_access_log_s *log);

/* log == NULL -> no-op. remote_user may be NULL/empty (becomes "-").
 * status < 0 -> "-" instead of a code (for example, the connection failed
 * before a response). */
void fpm_http_access_log_write(struct fpm_http_access_log_s *log, const char *remote_addr,
	const char *remote_user, const char *method, const char *uri, int http_major, int http_minor,
	int status, size_t bytes_sent, const char *referer, const char *user_agent);

#endif
