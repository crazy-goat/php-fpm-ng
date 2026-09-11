/* fpm-ng: access.log for pool.type = http-direct (issue #59).
 *
 * Why not upstream fpm_log.c, which already renders access.format and which
 * every fastcgi pool uses: two of its specifiers read state that only exists
 * under the FastCGI transport.
 *
 * - %e{VAR} does fcgi_getenv((fcgi_request *) SG(server_context), ...). Under
 *   this transport SG(server_context) is a struct fpm_direct_request
 *   (fpm_http_direct.c), so the cast reads a foreign object -- a crash, not a
 *   wrong value.
 * - %R does fcgi_get_last_client_ip(), a global that fastcgi.c fills at
 *   accept() time. Nothing in a direct pool ever writes it, so every line
 *   would carry whatever the process last did over FastCGI, or "-".
 *
 * The rest of the specifiers are rendered from the same places upstream reads
 * them, so a format that works for a fastcgi pool means the same thing here.
 * The format is still VALIDATED by upstream (fpm_conf.c calls fpm_log_write()
 * in test mode for every pool that sets access.log), which is deliberate: one
 * parser decides what a valid access.format is, and this file's job is only to
 * render an already-accepted one.
 *
 * The descriptor is wp->log_fd, opened by the master in fpm_log_open() before
 * any fork and re-opened in place on SIGUSR1 (fpm_log_open(1) dup2()s the new
 * file over the same descriptor and SIGQUITs the children). So rotation works
 * for a direct pool exactly as it does for a fastcgi one, and this file has
 * nothing to do about it.
 *
 * One write() per line on an O_APPEND descriptor is atomic against the other
 * children of the pool (POSIX), which is why no lock is taken -- the same
 * argument fpm_http_access_log.h makes for the gateway.
 */

#ifndef FPM_HTTP_DIRECT_ACCESS_LOG_H
#define FPM_HTTP_DIRECT_ACCESS_LOG_H 1

#include <sys/time.h>
#include <stddef.h>

struct evkeyvalq;
struct evhttp_request;
struct fpm_worker_pool_s;

/* Everything one line can need. A field left at its zero value renders as the
 * "nothing to say" form upstream uses for it ("-" for strings, 0 for counts),
 * so a response that never reached PHP -- a static file, ping, status, a
 * refusal -- can be logged by filling in only what it actually knows. */
struct fpm_http_direct_access_entry {
	const char *method;
	const char *uri;		/* path only, without the query string */
	const char *query_string;	/* without the '?' */
	const char *script_filename;
	const char *remote_addr;
	const char *remote_user;
	size_t content_length;		/* request body bytes */
	size_t bytes_sent;		/* response body bytes */
	int status;
	struct timeval started;		/* for %d, when duration is left at zero */
	time_t started_epoch;		/* for %t */
	struct timeval duration;	/* for %d; zero means "measure from started" */
	double cpu_percent;		/* for %C; a response that ran no PHP passes 0 */
	size_t memory;			/* PHP peak for this request, 0 when no PHP ran */
	/* CGI variables for %e{...}; NULL when no PHP request was built. */
	const struct evkeyvalq *env;
	/* Response headers for %o{...}, read while the request is still alive --
	 * hence the contract that a caller logs before or during the send, never
	 * from a completion callback. */
	struct evhttp_request *http;
};

struct fpm_http_direct_access_log_s;

/* NULL when the pool sets no access.log, which every caller treats as "do not
 * log" rather than an error. The returned handle is owned by the child and
 * lives until it exits. */
struct fpm_http_direct_access_log_s *fpm_http_direct_access_log_init_child(struct fpm_worker_pool_s *wp);

void fpm_http_direct_access_log_free(struct fpm_http_direct_access_log_s *log);

/* No-op when log is NULL or the URI matches an access.suppress_path entry. */
void fpm_http_direct_access_log_write(struct fpm_http_direct_access_log_s *log,
	const struct fpm_http_direct_access_entry *entry);

#endif
