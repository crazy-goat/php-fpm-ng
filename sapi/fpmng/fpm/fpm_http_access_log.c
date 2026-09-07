/* fpm-ng: see fpm_http_access_log.h. */

#include "fpm_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "fpm_http_access_log.h"
#include "zlog.h"

struct fpm_http_access_log_s {
	int fd;
};

struct fpm_http_access_log_s *fpm_http_access_log_open(const char *pool, const char *path) /* {{{ */
{
	struct fpm_http_access_log_s *log;
	int fd;

	if (!path || !*path) {
		return NULL;
	}

	fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (fd < 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.access_log: cannot open '%s': %s", pool, path, strerror(errno));
		return NULL;
	}

	log = malloc(sizeof(*log));
	if (!log) {
		close(fd);
		return NULL;
	}
	log->fd = fd;
	return log;
}
/* }}} */

void fpm_http_access_log_close(struct fpm_http_access_log_s *log) /* {{{ */
{
	if (!log) {
		return;
	}
	close(log->fd);
	free(log);
}
/* }}} */

/* Escape quotes, backslashes, and control characters — this is a log, not a
 * protocol, so truncate instead of reporting an error. Returns the length
 * written to `out` (without the final NUL). */
static size_t fpm_http_access_log_escape(const char *in, char *out, size_t out_size) /* {{{ */
{
	size_t o = 0;

	if (!in || out_size == 0) {
		if (out_size) {
			out[0] = '\0';
		}
		return 0;
	}
	for (; *in && o + 1 < out_size; in++) {
		unsigned char c = (unsigned char) *in;

		if (c == '"' || c == '\\' || c < 0x20 || c == 0x7f) {
			if (o + 2 >= out_size) {
				break;
			}
			out[o++] = '\\';
			out[o++] = (c == '"' || c == '\\') ? (char) c : '?';
		} else {
			out[o++] = (char) c;
		}
	}
	out[o] = '\0';
	return o;
}
/* }}} */

void fpm_http_access_log_write(struct fpm_http_access_log_s *log, const char *remote_addr, /* {{{ */
		const char *remote_user, const char *method, const char *uri, int http_major, int http_minor,
		int status, size_t bytes_sent, const char *referer, const char *user_agent)
{
	char line[4096];
	char uri_esc[1024], referer_esc[512], ua_esc[512];
	char addr_esc[128], user_esc[256];
	char status_buf[8];
	char timebuf[64];
	time_t now;
	struct tm tmv;
	int len;
	size_t total;
	ssize_t written;

	if (!log) {
		return;
	}

	now = time(NULL);
	localtime_r(&now, &tmv);
	strftime(timebuf, sizeof(timebuf), "%d/%b/%Y:%H:%M:%S %z", &tmv);

	/* remote_user comes from base64 in the Authorization header, so it may
	 * contain ANY bytes, including a newline — without escaping, a client could
	 * inject its own lines into the access log. Escape remote_addr as well. */
	fpm_http_access_log_escape(remote_addr, addr_esc, sizeof(addr_esc));
	fpm_http_access_log_escape(remote_user, user_esc, sizeof(user_esc));
	fpm_http_access_log_escape(uri, uri_esc, sizeof(uri_esc));
	fpm_http_access_log_escape(referer, referer_esc, sizeof(referer_esc));
	fpm_http_access_log_escape(user_agent, ua_esc, sizeof(ua_esc));

	if (status >= 0) {
		snprintf(status_buf, sizeof(status_buf), "%d", status);
	} else {
		snprintf(status_buf, sizeof(status_buf), "-");
	}

	len = snprintf(line, sizeof(line),
		"%s - %s [%s] \"%s %s HTTP/%d.%d\" %s %zu \"%s\" \"%s\"\n",
		addr_esc[0] ? addr_esc : "-",
		user_esc[0] ? user_esc : "-",
		timebuf,
		method ? method : "-",
		uri_esc[0] ? uri_esc : "-",
		http_major, http_minor,
		status_buf,
		bytes_sent,
		referer_esc[0] ? referer_esc : "-",
		ua_esc[0] ? ua_esc : "-");

	if (len <= 0) {
		return;
	}
	total = (size_t) len >= sizeof(line) ? sizeof(line) - 1 : (size_t) len;

	/* DELIBERATELY one write(): this guarantees that lines from other gateway
	 * processes do not interleave in the same O_APPEND file (see the header).
	 * A short write (almost always ENOSPC on a regular file) may truncate the
	 * line, but do not append the rest with a second write() — that would break
	 * exactly this guarantee. */
	do {
		written = write(log->fd, line, total);
	} while (written < 0 && errno == EINTR);
}
/* }}} */
