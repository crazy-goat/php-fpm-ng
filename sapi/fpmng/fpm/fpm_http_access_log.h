/* fpm-ng: HTTP gateway access log (http.access_log), Combined Log Format,
 * not configurable — see docs/NOTES.md for why (simplicity is valuable here).
 *
 * Every http.gateways gateway process has its own descriptor for THE SAME file,
 * opened with O_APPEND. One write() per line on a regular O_APPEND file is
 * atomic relative to other writers (POSIX), so lines from gateway processes do
 * not interleave despite having no shared lock. Writes are synchronous (as in
 * nginx/Apache) — a local disk/page cache makes this cheap, so an asynchronous
 * buffering layer is not worth the complexity.
 *
 * Rotation: unlike the error_log, this file is opened by the gateway process
 * itself, after the privilege drop, so the process that has to reopen it can
 * also do so — see fpm_http_access_log_reopen() below (issue #137).
 *
 * The trap that follows from reopening by path: the gateway runs as the pool's
 * user, so a logrotate stanza whose `create` mode/owner excludes that user
 * (`create 0640 root adm`, say) makes the reopen fail with EACCES and leaves
 * the process appending to the rotated inode. `copytruncate`, or a `create`
 * the pool's user can write, is the operator-side answer.
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

/* Gateway process, from its own event loop: open `path` again after a
 * logrotate, so this process stops appending to the renamed or deleted file
 * (issue #137). Returns the handle to keep:
 *
 * - `log` itself when the reopen succeeded, and also when it failed — a
 *   rotation must not turn logging off, and the previous file is still on disk;
 * - a freshly opened handle when `log` was NULL because the open at startup
 *   had failed (a rotation is the one moment retrying is worth it: the
 *   operator has just been in that directory);
 * - NULL when logging is disabled, i.e. `log` was NULL and so is `path`.
 *
 * The gateway can do this by itself only because it owns the file: it opens
 * the access log after fpm_http_gateway_drop_privileges(), so the file belongs
 * to the dropped-to identity — which is exactly what is not true of the
 * error_log, and why that one is handed over as a descriptor instead
 * (fpm_error_log_follow.h). What the gateway cannot do is notice the
 * rotation: SIGUSR1 reaches the master only. The wakeup arrives over the
 * follow channel of that same header. */
struct fpm_http_access_log_s *fpm_http_access_log_reopen(struct fpm_http_access_log_s *log,
	const char *pool, const char *path);

/* log == NULL -> no-op. remote_user may be NULL/empty (becomes "-").
 * status < 0 -> "-" instead of a code (for example, the connection failed
 * before a response). */
void fpm_http_access_log_write(struct fpm_http_access_log_s *log, const char *remote_addr,
	const char *remote_user, const char *method, const char *uri, int http_major, int http_minor,
	int status, size_t bytes_sent, const char *referer, const char *user_agent);

#endif
