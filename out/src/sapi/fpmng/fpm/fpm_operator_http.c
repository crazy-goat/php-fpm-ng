/* fpm-ng: the raw HTTP server behind every operator endpoint.
 *
 * See fpm_operator_http.h for why there is exactly one of these and why it is
 * this small. The code here was pool.type = status's accept loop until issue
 * #274; the only change of substance is that the two hard-coded paths became a
 * dispatch callback.
 */

#include "fpm_config.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "fpm_operator_http.h"

/* Per-connection recv()/send() limit -- see the rationale next to setsockopt()
 * in fpm_operator_http_serve(). */
#define FPM_OPERATOR_HTTP_IO_TIMEOUT_SEC 5

void fpm_operator_buf_free(struct fpm_operator_buf_s *b) /* {{{ */
{
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}
/* }}} */

void fpm_operator_buf_appendf(struct fpm_operator_buf_s *b, const char *fmt, ...) /* {{{ */
{
	for (;;) {
		size_t avail = b->cap - b->len;
		va_list ap;
		int n;

		va_start(ap, fmt);
		n = vsnprintf(b->data ? b->data + b->len : NULL, avail, fmt, ap);
		va_end(ap);

		if (n < 0) {
			return;
		}
		if ((size_t) n < avail) {
			b->len += (size_t) n;
			return;
		}

		{
			size_t new_cap = b->cap ? b->cap * 2 : 256;
			char *new_data;

			while (new_cap < b->len + (size_t) n + 1) {
				new_cap *= 2;
			}
			new_data = realloc(b->data, new_cap);
			if (!new_data) {
				return;
			}
			b->data = new_data;
			b->cap = new_cap;
		}
	}
}
/* }}} */

/* One response, written in full but under a bounded total time. write() is
 * warn_unused_result in glibc and a (void) cast does not satisfy that (task
 * 013's lint job reported both call sites below), so the result is used for
 * something worth doing anyway: the fd is a blocking socket (nothing here or
 * in fpm_sockets.c sets O_NONBLOCK, and accept() does not inherit it on
 * Linux), so a short write means a signal arrived mid-write, and returning
 * there would have truncated a /metrics response. Unlike the access log
 * (fpm_http_access_log.c:146-153), nothing here depends on the payload
 * reaching the fd in a single write() -- one connection carries exactly one
 * response and is then closed.
 *
 * The deadline is why this is not a plain retry loop. SO_SNDTIMEO
 * (FPM_OPERATOR_HTTP_IO_TIMEOUT_SEC, set per connection in fpm_operator_http_serve()) is per
 * write() call, not per response, and the pool behind an operator endpoint always has
 * pm.max_children = 1, so a client that reads one byte every 4 seconds would make
 * partial progress on every call, restart the 5s timer each time, and pin the
 * only process for as long as it cares to trickle -- exactly the hang the
 * setsockopt() block was added to prevent. A single write() used to bound
 * that implicitly; retrying has to bound it explicitly. Budget is one
 * SO_SNDTIMEO's worth for the whole response, measured on CLOCK_MONOTONIC so
 * a clock step cannot extend it.
 *
 * A peer that hung up, or ran out the budget, is not logged: this endpoint is
 * scraped every 15-60s and a scraper that gives up mid-response would
 * otherwise fill the error log. */
static void fpm_operator_http_write_all(int fd, const char *data, size_t len) /* {{{ */
{
	struct timespec deadline;

	if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
		/* No clock, no bound we can honour -- one write() and be done, which
		 * is the behaviour this function replaced. */
		if (write(fd, data, len) < 0) {
			return;
		}
		return;
	}
	deadline.tv_sec += FPM_OPERATOR_HTTP_IO_TIMEOUT_SEC;

	while (len) {
		struct timespec now;
		ssize_t n = write(fd, data, len);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return;
		}
		if (n == 0) {
			return;
		}
		data += n;
		len -= (size_t) n;

		if (len && clock_gettime(CLOCK_MONOTONIC, &now) == 0
			&& (now.tv_sec > deadline.tv_sec
				|| (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))) {
			return;
		}
	}
}
/* }}} */

int fpm_operator_http_has_flag(const char *query, const char *flag) /* {{{ */
{
	const char *p = query;
	size_t want = strlen(flag);

	while (p && *p) {
		size_t len = strcspn(p, "&");

		if (len == want && !strncmp(p, flag, want)) {
			return 1;
		}
		p += len;
		if (*p == '&') {
			p++;
		}
	}
	return 0;
}
/* }}} */

static void fpm_operator_http_handle_conn(int fd, fpm_operator_http_dispatch_cb cb, void *ctx,
	const char *known_paths) /* {{{ */
{
	char req[4096];
	ssize_t n;
	size_t total = 0;
	char path[256] = "";
	char *query;
	struct fpm_operator_reply_s reply;
	int status_code = 200;
	const char *status_text = "OK";
	char header[512];
	int header_len;

	memset(&reply, 0, sizeof(reply));
	reply.content_type = "text/plain; charset=utf-8";

	/* One or more recv() calls, until we see the end of the first line or fill
	 * the buffer -- sufficient; "GET /metrics HTTP/1.1\r\n" fits with ample room.
	 * The rest of the request (headers and any body) is ignored -- we do not parse
	 * it anyway. */
	while (total < sizeof(req) - 1) {
		n = recv(fd, req + total, sizeof(req) - 1 - total, 0);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		total += (size_t) n;
		req[total] = '\0';
		if (strstr(req, "\r\n") || strstr(req, "\n")) {
			break;
		}
	}

	if (total == 0) {
		return;
	}
	req[total] = '\0';

	{
		char method[16];
		if (sscanf(req, "%15s %255s", method, path) != 2) {
			path[0] = '\0';
		}
	}

	/* "/status?json&full" is the status page asked for a variant, not a path
	 * nobody configured. The path is matched whole against the routes, so the
	 * query has to come off it first. */
	query = strchr(path, '?');
	if (query) {
		*query++ = '\0';
	}

	if (path[0]) {
		cb(ctx, path, query ? query : "", &reply);
	}

	if (!reply.handled) {
		fpm_operator_buf_free(&reply.body);
		reply.content_type = "text/plain; charset=utf-8";
		status_code = 404;
		status_text = "Not Found";
		fpm_operator_buf_appendf(&reply.body, "not found: %s\n",
			path[0] ? path : "(unparseable request)");
		if (known_paths && *known_paths) {
			fpm_operator_buf_appendf(&reply.body, "known paths: %s\n", known_paths);
		}
	}

	/* A monitoring page a proxy is free to cache is a monitoring page that
	 * lies; upstream's fpm_status.c sends the same two, and so did the
	 * http-direct status page on its own listener before issue #275 moved it
	 * here. Sent on the 404 as well -- a path that is wrong now is not
	 * permanently wrong, and a cached one would hide the fix. */
	header_len = snprintf(header, sizeof(header),
		"HTTP/1.1 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %zu\r\n"
		"Expires: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
		"Cache-Control: no-cache, no-store, must-revalidate, max-age=0\r\n"
		"Connection: close\r\n"
		"\r\n",
		status_code, status_text, reply.content_type, reply.body.len);

	if (header_len > 0) {
		fpm_operator_http_write_all(fd, header, (size_t) header_len);
	}
	if (reply.body.data && reply.body.len) {
		fpm_operator_http_write_all(fd, reply.body.data, reply.body.len);
	}

	fpm_operator_buf_free(&reply.body);
}
/* }}} */

void fpm_operator_http_serve(int listen_fd, fpm_operator_http_dispatch_cb cb, void *ctx,
	const char *known_paths) /* {{{ */
{
	/* Deliberately no custom signal handling: SIGTERM has its default
	 * disposition here (fpm_signals_child_init() sets it for children before
	 * run_child:) -- the process simply exits immediately, which is correct
	 * because an operator endpoint has no "current work" to complete (each
	 * connection is fully handled in one accept() cycle and never lasts longer
	 * than one recv/send). On reload (SIGUSR2), fpm_process_ctl.c sends SIGTERM
	 * to such a pool directly for exactly that reason -- see the comment at
	 * fpm_process_ctl.c:165 and docs/NOTES.md 3x. Outside of reload -- an
	 * explicit graceful stop or log rotation -- the ordinary SIGQUIT fan-out
	 * still applies, and a delay until the master escalates to SIGTERM there is
	 * known and accepted, the same as for supervisor/cron without a custom
	 * SIGQUIT handler -- see docs/NOTES.md 3p, scenario 2. */

	for (;;) {
		int fd = accept(listen_fd, NULL, NULL);
		struct timeval tv;

		if (fd < 0) {
			if (errno == EINTR) {
				continue;
			}
			/* accept() errors on TCP/UDS sockets are usually transient (for
			 * example, ECONNABORTED) -- there is no reason to terminate the entire
			 * process because of one failed connection. */
			continue;
		}

		/* A client that opens a connection and never sends anything (or does not
		 * read the response) would hang this pool's ONLY process forever --
		 * pm.max_children is always 1 here, so there is no other worker to take
		 * over traffic meanwhile. Timeout both recv() AND send(), not only
		 * accept(). */
		tv.tv_sec = FPM_OPERATOR_HTTP_IO_TIMEOUT_SEC;
		tv.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		fpm_operator_http_handle_conn(fd, cb, ctx, known_paths);
		close(fd);
	}
}
/* }}} */
