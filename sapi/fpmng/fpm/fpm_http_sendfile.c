/* X-Accel-Redirect and X-Sendfile from an upstream reply (issue #649). See
 * fpm_http_sendfile.h for what this does and does not do.
 *
 * The file is served through the same containment rules as http.static
 * (fpm_http_static.c): the path is resolved with realpath(), it must lie at or
 * below the root, a dot-segment below the root is refused, a PHP script is
 * refused, and only a regular file is opened. The check runs on the path the
 * gateway resolves, not on the header's text, so a symlink inside the root that
 * points outside it is refused as well.
 *
 * The body is read in windows of FPM_HTTP_SENDFILE_WINDOW bytes. The next
 * window is read only after libevent has written the previous one to the
 * client (evhttp_send_reply_chunk_with_cb()), so the gateway holds one window
 * per file reply, not the file. evbuffer_add_file() is not used for this: a
 * buffer without EVBUFFER_FLAG_DRAINS_TO_FD maps the whole range of the file
 * (libevent 2.1.12, evbuffer_file_segment_materialize()), so the size of the
 * file would be the size of the gateway's resident memory. The upstream's own
 * body is discarded (discard_upstream), and the upstream's end does not end a
 * file reply: fpm_http_finish() waits for the last window to drain.
 *
 * Each window is read with pread() at the offset the previous window ended at,
 * into a buffer of its own. No mapping of the file exists, so a truncation
 * during the transfer cannot raise SIGBUS in the gateway: pread() returns 0 or
 * fails, and the reply ends as a cut reply (fpm_http_sendfile_end(), issue
 * #533). The connection is shut down, because the Content-Length cannot be met.
 */

#include "fpm_config.h"

#include "fpm.h"
#include "fpm_http_sendfile.h"

#ifdef HAVE_FPM_HTTP

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include "fpm_http_internal.h"
#include "fpm_http_direct_request.h"
#include "fpm_http_static.h"
#include "zlog.h"

#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif

/* One window of a file reply. A window is what one client drain lets the
 * gateway hold for this reply: 1 MiB, so a 200 MB file takes 200 drains. */
#define FPM_HTTP_SENDFILE_WINDOW (1024 * 1024)

/* The upstream headers a served file does not keep. Content-Length,
 * Content-Range, Transfer-Encoding and Content-Encoding describe the worker's
 * body, which the file replaces: a worker's Content-Encoding would tell the
 * client to decode the raw file. Accept-Ranges is dropped because the gateway
 * answers no range (a client would ask for one). The others belong to the
 * connection between the worker and the gateway (RFC 9110, section 7.6.1), and
 * X-Request-Id is the gateway's own (issue #642). */
static const char *const fpm_http_sendfile_dropped[] = {
	"Content-Length", "Content-Range", "Transfer-Encoding", "Content-Encoding",
	"Accept-Ranges", "X-Request-Id",
	"Connection", "Keep-Alive", "Proxy-Connection", "Proxy-Authenticate",
	"Proxy-Authorization", "TE", "Trailer", "Upgrade",
	"X-Accel-Redirect", "X-Sendfile", NULL
};

int fpm_http_sendfile_note(fpm_http_conn *c, const char *key, const char *value, size_t vlen)
{
	struct fpm_http_sendfile_s *sf;
	const char *header, *end;
	int absolute;

	if (strcasecmp(key, "X-Accel-Redirect") == 0) {
		header = "X-Accel-Redirect";
		absolute = 0;
	} else if (strcasecmp(key, "X-Sendfile") == 0) {
		header = "X-Sendfile";
		absolute = 1;
	} else {
		return 0;
	}

	/* Two redirect headers in one reply: the first decides. The second is
	 * dropped, not refused, so that the outcome does not depend on the order
	 * the upstream sent them in beyond the first one. */
	if (c->sendfile) {
		zlog(ZLOG_NOTICE, "[pool %s] http: upstream sent a second %s header, ignored", c->gw->pool, header);
		return 1;
	}
	sf = calloc(1, sizeof(*sf));
	if (!sf) {
		zlog(ZLOG_WARNING, "[pool %s] http: cannot copy an upstream %s header (out of memory), answering 502",
				c->gw->pool, header);
		return -1;
	}
	sf->fd = -1; /* before any free: a zeroed fd would be stdin */
	end = value + vlen;
	while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
		end--;
	}
	sf->path = strndup(value, end - value);
	if (!sf->path) {
		free(sf);
		zlog(ZLOG_WARNING, "[pool %s] http: cannot copy an upstream %s header (out of memory), answering 502",
				c->gw->pool, header);
		return -1;
	}
	sf->header = header;
	sf->absolute = absolute;
	c->sendfile = sf;
	return 1;
}

static void fpm_http_sendfile_close(struct fpm_http_sendfile_s *sf)
{
	if (sf->fd >= 0) {
		close(sf->fd);
		sf->fd = -1;
	}
}

/* Removes the upstream headers that a served file does not keep. Each name is
 * removed in full, as evhttp_remove_header() takes out one copy per call. */
static void fpm_http_sendfile_strip(struct evkeyvalq *out)
{
	const char *const *name;

	for (name = fpm_http_sendfile_dropped; *name; name++) {
		while (evhttp_remove_header(out, *name) == 0) {
			;
		}
	}
}

/* Reads the next window of the body into a new buffer, at sf->offset. NULL
 * when the file ends before the length it had, or cannot be read, or when
 * memory runs out. The caller then ends the reply (see fpm_http_sendfile_end()).
 * pread() takes the offset, so the file position is not used, and a short
 * file is an error here, not a mapping that faults. */
static struct evbuffer *fpm_http_sendfile_next(fpm_http_conn *c)
{
	struct fpm_http_sendfile_s *sf = c->sendfile;
	struct evbuffer *win = evbuffer_new();
	struct evbuffer_iovec iov;
	size_t want, got = 0;

	if (!win) {
		return NULL;
	}
	want = sf->left < FPM_HTTP_SENDFILE_WINDOW ? (size_t) sf->left : FPM_HTTP_SENDFILE_WINDOW;
	if (evbuffer_reserve_space(win, want, &iov, 1) != 1) {
		evbuffer_free(win);
		return NULL;
	}
	while (got < want) {
		ssize_t n = pread(sf->fd, (char *) iov.iov_base + got, want - got, sf->offset + (off_t) got);

		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			evbuffer_free(win);
			return NULL;
		}
		got += (size_t) n;
	}
	iov.iov_len = got;
	if (evbuffer_commit_space(win, &iov, 1) != 0) {
		evbuffer_free(win);
		return NULL;
	}
	sf->offset += (off_t) got;
	sf->left -= (off_t) got;
	return win;
}

static void fpm_http_sendfile_drained(struct evhttp_connection *evcon, void *arg);

/* Hands one window to libevent. The client's drain of it runs
 * fpm_http_sendfile_drained(). The caller must not touch c after this call:
 * libevent may run the drain callback before the call returns. */
static void fpm_http_sendfile_send(fpm_http_conn *c, struct evbuffer *win)
{
	c->bytes_out += evbuffer_get_length(win);
	evhttp_send_reply_chunk_with_cb(c->req, win, fpm_http_sendfile_drained, c->client);
	evbuffer_free(win);
}

/* The last window has drained, or the file ended or failed while it was sent.
 * A file that ended early is cut, not terminated (issue #533): the client must
 * not take a short body for a complete one. */
static void fpm_http_sendfile_end(fpm_http_conn *c)
{
	struct fpm_http_sendfile_s *sf = c->sendfile;
	int truncated = sf->left > 0;
	char escaped_path[4 * 64 + 1];

	sf->pumping = 0;
	fpm_http_sendfile_close(sf);
	if (truncated) {
		zlog(ZLOG_WARNING, "[pool %s] http: cannot read the rest of '%s' for %s, the reply is cut short",
				c->gw->pool, fpm_http_log_escape(sf->path, 64, escaped_path, sizeof(escaped_path)), sf->header);
		c->chunk_lost = 1;
	}
	/* A paused upstream is resumed while c still names it: resume reads
	 * c->upstream, and the detach below would leave it paused. The call is
	 * idempotent, so fpm_http_finish() repeats it without effect. */
	fpm_http_response_resume(c);
	/* The worker may still be writing its own body (discard_upstream), so the
	 * upstream stays up, but it must not keep pointing at c once c is freed:
	 * fpm_http_finish() frees it, and the upstream's next read would reach
	 * up->current (heap-use-after-free). Detached the way fpm_http_client_closed()
	 * detaches a client that went away; the upstream reads and drops the rest of
	 * the worker's reply with no request, like a reply whose client has gone. */
	if (c->upstream) {
		c->upstream->current = NULL;
		c->upstream = NULL;
	}
	fpm_http_finish(c, 1);
}

/* libevent runs this when the client has taken the window it was sent with.
 * It sends the next window, or ends the reply. */
static void fpm_http_sendfile_drained(struct evhttp_connection *evcon, void *arg)
{
	struct fpm_http_client_s *cl = arg;
	fpm_http_conn *c = cl->c;
	struct evbuffer *win;

	(void) evcon;
	/* cl->c is NULL once the client has gone (fpm_http_client_closed()). */
	if (!c || !c->sendfile || !c->sendfile->pumping) {
		return;
	}
	if (c->sendfile->left > 0 && (win = fpm_http_sendfile_next(c)) != NULL) {
		fpm_http_sendfile_send(c, win);
		return;
	}
	fpm_http_sendfile_end(c);
}

void fpm_http_sendfile_reply(fpm_http_conn *c)
{
	struct fpm_http_sendfile_s *sf = c->sendfile;
	struct evkeyvalq *out = evhttp_request_get_output_headers(c->req);
	const char *root = c->gw->sendfile_root, *refusal = NULL, *reason = "OK", *type = "text/plain", *rel;
	char root_buf[MAXPATHLEN], candidate[MAXPATHLEN], resolved[MAXPATHLEN], why[160], clen[32];
	struct stat stbuf;
	struct evbuffer *body = NULL;
	size_t root_len = 0, len;
	off_t length = 0;
	int fd = -1, code = HTTP_OK, n, head, pump = 0;

	head = evhttp_request_get_command(c->req) == EVHTTP_REQ_HEAD;

	/* realpath() on every use: a root that is a symlink can move (a
	 * `current -> releases/N` deployment, as in issue #638), and a request
	 * with no redirect header never gets here. */
	if (!realpath(root, root_buf)) {
		refusal = "the root does not resolve";
	} else {
		root_len = strlen(root_buf);
	}

	if (!refusal && sf->path[0] != '/') {
		refusal = "not an absolute path";
	}
	if (!refusal) {
		/* X-Sendfile names a filesystem path; X-Accel-Redirect names a path
		 * below the root, which is what nginx means by the same header. */
		n = sf->absolute ? snprintf(candidate, sizeof(candidate), "%s", sf->path)
						 : snprintf(candidate, sizeof(candidate), "%s%s", root_buf, sf->path);
		if (n < 0 || (size_t) n >= sizeof(candidate)) {
			refusal = "the path is too long";
		}
	}
	if (!refusal && !realpath(candidate, resolved)) {
		refusal = "no such file";
	}
	if (!refusal && !fpm_http_static_contained(resolved, root_buf, root_len)) {
		refusal = "outside the root";
	}
	/* The dot-segment rule of http.static, applied to the part below the root:
	 * the root's own path may contain a dot-segment, a file below it may not. */
	if (!refusal) {
		rel = resolved + root_len;
		if (fpm_http_static_path_has_dotfile(rel)) {
			refusal = "a dot-segment in the path";
		}
	}
	if (!refusal) {
		len = strlen(resolved);
		if (len >= 4 && strcasecmp(resolved + len - 4, ".php") == 0) {
			refusal = "a PHP script";
		}
	}
	if (!refusal) {
		/* The flags of fpm_http_static_open_regular(), which cannot say why a
		 * step failed. The errno goes to the log. */
		fd = open(resolved, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
		if (fd < 0) {
			snprintf(why, sizeof(why), "cannot open the file: %s", strerror(errno));
			refusal = why;
		} else if (fstat(fd, &stbuf) < 0) {
			snprintf(why, sizeof(why), "cannot stat the file: %s", strerror(errno));
			close(fd);
			fd = -1;
			refusal = why;
		} else if (!S_ISREG(stbuf.st_mode)) {
			close(fd);
			fd = -1;
			refusal = "not a regular file";
		}
	}

	if (refusal) {
		char escaped_path[4 * 64 + 1];

		zlog(ZLOG_NOTICE, "[pool %s] http: refused %s '%s': %s", c->gw->pool, sf->header,
				fpm_http_log_escape(sf->path, 64, escaped_path, sizeof(escaped_path)), refusal);
		code = HTTP_NOTFOUND;
		reason = "Not Found";
		length = 10;
		if (!head) {
			body = evbuffer_new();
			if (body) {
				evbuffer_add(body, "Not Found\n", 10);
			} else {
				length = 0; /* out of memory: a 404 with no body, and the length says so */
			}
		}
	} else {
		type = fpm_http_static_content_type(resolved);
		length = stbuf.st_size;
		sf->fd = fd;
		sf->left = length;
		/* A HEAD reply carries the length and no body. An empty file has no
		 * body either, and the window reader needs a length to read. */
		if (head || length == 0) {
			fpm_http_sendfile_close(sf);
		} else {
			pump = 1;
		}
	}

	/* The first window is read before the headers go out, so a file that
	 * cannot be read still gets a 502. */
	if (pump) {
		body = c->client ? fpm_http_sendfile_next(c) : NULL;
		if (!body) {
			zlog(ZLOG_WARNING, "[pool %s] http: cannot read '%s' for %s (out of memory or the file changed), answering 502",
					c->gw->pool, resolved, sf->header);
			fpm_http_sendfile_close(sf);
			sf->left = 0;
			pump = 0;
			code = FPM_HTTP_BAD_GATEWAY;
			reason = "Bad Gateway";
			type = "text/plain";
			length = 0;
		}
	}

	/* A served file keeps the upstream's headers (Content-Disposition,
	 * Cache-Control, Set-Cookie and the rest), minus the ones that describe the
	 * worker's body. A refusal or a 502 keeps none: it is the gateway's answer.
	 * The request id is the exception: the client correlates the reply with it
	 * (issue #642). */
	if (code == HTTP_OK) {
		fpm_http_sendfile_strip(out);
		if (!evhttp_find_header(out, "Content-Type")) {
			evhttp_add_header(out, "Content-Type", type);
		}
	} else {
		evhttp_clear_headers(out);
		evhttp_add_header(out, "Content-Type", "text/plain");
	}
	snprintf(clen, sizeof(clen), "%lld", (long long) length);
	evhttp_add_header(out, "Content-Length", clen);
	if (c->client && c->client->request_id[0]) {
		evhttp_add_header(out, "X-Request-Id", c->client->request_id);
	}
	/* http.pool_full_policy = wait (issue #309): the same header the normal
	 * reply carries, added here because this reply returns before that code. */
	if (c->queue_wait_ms >= 0) {
		char waited[32];

		snprintf(waited, sizeof(waited), "%ld", c->queue_wait_ms);
		evhttp_add_header(out, "X-Fpmng-Queue-Wait", waited);
	}

	/* Everything on c is set before the first window is sent: libevent may run
	 * the drain callback inside that call, and the callback can end the
	 * request and free c (see fpm_http_finish()). */
	c->status = code;
	c->headers_sent = 1;
	c->discard_upstream = 1;
	smart_str_free(&c->cgi_headers);
	if (pump) {
		sf->pumping = 1;
	}
	evhttp_send_reply_start(c->req, code, reason);
	if (pump) {
		fpm_http_sendfile_send(c, body);
		return;
	}
	if (body) {
		c->bytes_out += evbuffer_get_length(body);
		evhttp_send_reply_chunk(c->req, body);
		evbuffer_free(body);
	}
}

void fpm_http_sendfile_free(struct fpm_http_sendfile_s *sf)
{
	if (!sf) {
		return;
	}
	fpm_http_sendfile_close(sf);
	free(sf->path);
	free(sf);
}

int fpm_http_sendfile_validate(const char *pool, const char *root)
{
	struct stat stbuf;
	char resolved[MAXPATHLEN];

	if (!root) {
		return 0;
	}
	if (root[0] != '/') {
		zlog(ZLOG_ERROR, "[pool %s] http.sendfile_root must be an absolute path, got '%s'", pool, root);
		return -1;
	}
	if (stat(root, &stbuf) < 0 || !S_ISDIR(stbuf.st_mode)) {
		zlog(ZLOG_ERROR, "[pool %s] http.sendfile_root '%s' is not an existing directory", pool, root);
		return -1;
	}
	/* fpm_http_static_contained() needs a '/' after the root, so with "/" every
	 * path is refused and the directive serves nothing. The resolved path is
	 * checked, so "//" and "/." are refused as well. */
	if (realpath(root, resolved) && strcmp(resolved, "/") == 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.sendfile_root must not be '/', name the directory that holds the files to serve", pool);
		return -1;
	}
	return 0;
}

#endif /* HAVE_FPM_HTTP */
