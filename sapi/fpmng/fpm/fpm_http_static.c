/* Files from disk, served without a PHP request. See fpm_http_static.h for why
 * this is one module and not one per caller. */

#include "fpm_config.h"

#include "fpm.h"
#include "fpm_http_static.h"

#ifdef HAVE_FPM_HTTP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include "fpm_http_static_negotiate.h"
#include "fpm_http_static_range.h"
#include "zlog.h"

#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif

#define FPM_HTTP_STATIC_BAD_GATEWAY 502 /* libevent has no constant for it */
#define FPM_HTTP_STATIC_NOT_SATISFIABLE 416 /* libevent has no constant for it either */
#define FPM_HTTP_STATIC_PARTIAL 206 /* and none for this one */

static const struct {
	const char *ext;
	const char *type;
} fpm_http_static_mime[] = {
	{ "html", "text/html; charset=UTF-8" },
	{ "htm",  "text/html; charset=UTF-8" },
	{ "css",  "text/css; charset=UTF-8" },
	{ "js",   "text/javascript; charset=UTF-8" },
	{ "mjs",  "text/javascript; charset=UTF-8" },
	{ "json", "application/json" },
	{ "map",  "application/json" },
	{ "xml",  "application/xml" },
	{ "txt",  "text/plain; charset=UTF-8" },
	{ "svg",  "image/svg+xml" },
	{ "png",  "image/png" },
	{ "jpg",  "image/jpeg" },
	{ "jpeg", "image/jpeg" },
	{ "gif",  "image/gif" },
	{ "webp", "image/webp" },
	{ "avif", "image/avif" },
	{ "ico",  "image/x-icon" },
	{ "woff", "font/woff" },
	{ "woff2","font/woff2" },
	{ "ttf",  "font/ttf" },
	{ "otf",  "font/otf" },
	{ "pdf",  "application/pdf" },
	{ "wasm", "application/wasm" },
	{ "mp4",  "video/mp4" },
	{ "webm", "video/webm" },
	{ NULL, NULL }
};

static const char *fpm_http_static_known_type(const char *path)
{
	const char *dot = strrchr(path, '.');
	unsigned i;

	if (!dot || strchr(dot, '/')) {
		return NULL;
	}
	dot++;
	for (i = 0; fpm_http_static_mime[i].ext; i++) {
		if (!strcasecmp(dot, fpm_http_static_mime[i].ext)) {
			return fpm_http_static_mime[i].type;
		}
	}

	return NULL;
}

const char *fpm_http_static_content_type(const char *path)
{
	const char *type = fpm_http_static_known_type(path);

	return type ? type : "application/octet-stream";
}

char *fpm_http_static_decode_path(struct evhttp_request *req, size_t *len)
{
	const struct evhttp_uri *uri = evhttp_request_get_evhttp_uri(req);
	const char *raw = uri ? evhttp_uri_get_path(uri) : NULL;
	size_t path_len;
	char *path;

	if (!raw || !*raw) {
		return NULL;
	}
	path = evhttp_uridecode(raw, 0, &path_len);
	if (!path) {
		return NULL;
	}
	/* Decoding is what makes these checks worth making: %2e%2e%2f is a
	 * traversal the raw form does not show, and a %00 truncates every
	 * str*() that looks at the result afterwards. */
	if (path_len != strlen(path) || path[0] != '/' || strstr(path, "/../") ||
		(path_len >= 3 && !memcmp(path + path_len - 3, "/..", 3))) {
		free(path);
		return NULL;
	}
	*len = path_len;

	return path;
}

/* Any path segment starting with a dot is refused: .env, .git, .htaccess and
 * friends must never be served just because they sit under the document root.
 * nginx needs an explicit rule for this; we make it the default. */
static int fpm_http_static_path_has_dotfile(const char *path)
{
	const char *p = path;

	while ((p = strchr(p, '/')) != NULL) {
		if (p[1] == '.') {
			return 1;
		}
		p++;
	}

	return 0;
}

static void fpm_http_static_log(const struct fpm_http_static *st, int status, size_t bytes)
{
	if (st->log) {
		st->log(st->log_ctx, status, bytes);
	}
}

/* RFC 9110 IMF-fixdate, which is the only format a Last-Modified may use. In C
 * locale explicitly: strftime()'s %a and %b follow LC_TIME, and a pool running
 * under a non-English locale must not emit day names no client can parse. */
static void fpm_http_static_http_date(time_t when, char *out, size_t size)
{
	static const char *const days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	struct tm tm;

	if (!gmtime_r(&when, &tm)) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%s, %02d %s %04d %02d:%02d:%02d GMT",
		days[tm.tm_wday % 7], tm.tm_mday, months[tm.tm_mon % 12], tm.tm_year + 1900,
		tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* Only the one format above is accepted, compared as bytes against what we
 * ourselves sent. The two obsolete formats RFC 9110 still requires a *recipient*
 * to parse are deliberately not: a client that sends one gets the file, which
 * costs it a transfer and cannot make it wrong. Guessing at a date format and
 * getting it wrong is what would make it wrong. */
static int fpm_http_static_not_modified(struct evhttp_request *req, const char *etag,
		const char *last_modified)
{
	struct evkeyvalq *in = evhttp_request_get_input_headers(req);
	const char *value;

	value = evhttp_find_header(in, "If-None-Match");
	if (value) {
		/* Present and not a match means the client holds a different version:
		 * If-Modified-Since must not be consulted as well (RFC 9110 13.1.3). */
		return !strcmp(value, etag);
	}
	value = evhttp_find_header(in, "If-Modified-Since");

	return value && last_modified[0] && !strcmp(value, last_modified);
}

/* A refusal that does not take the connection with it. evhttp_send_error()
 * clears every header and forces Connection: close (http.c, evhttp_send_page_),
 * which for a file server means one probe for /.env tears down the keep-alive
 * connection carrying the rest of the page's assets. Nothing here needs the
 * connection closed: a refusal is an answer, and the next request on the same
 * connection is as valid as this one was. */
static void fpm_http_static_refuse(const struct fpm_http_static *st, struct evhttp_request *req)
{
	struct evbuffer *body = evbuffer_new();

	fpm_http_static_log(st, HTTP_NOTFOUND, 0);
	if (!body) {
		evhttp_send_error(req, HTTP_NOTFOUND, NULL);	/* out of memory: the connection is the least of it */
		return;
	}
	evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "text/plain");
	evbuffer_add(body, "Not Found\n", 10);
	evhttp_send_reply(req, HTTP_NOTFOUND, "Not Found", body);
	evbuffer_free(body);
}

/* The check every served path passes, the original and a sidecar alike: the
 * resolved path is the root itself or lies below it. */
static int fpm_http_static_contained(const char *resolved, const char *root, size_t root_len)
{
	return strncmp(resolved, root, root_len) == 0 && (!resolved[root_len] || resolved[root_len] == '/');
}

/* The open() flags and the S_ISREG test of the original (issue #1008), for a
 * file that is not the original. */
static int fpm_http_static_open_regular(const char *path, struct stat *stbuf)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);

	if (fd < 0) {
		return -1;
	}
	if (fstat(fd, stbuf) < 0 || !S_ISREG(stbuf->st_mode)) {
		close(fd);
		return -1;
	}

	return fd;
}

/* True when one Accept-Encoding line of the request accepts `coding`. A
 * request may repeat the header, and each line counts. */
static int fpm_http_static_header_accepts(const struct evkeyvalq *in, const char *coding)
{
	struct evkeyval *kv;

	TAILQ_FOREACH(kv, in, next) {
		if (!strcasecmp(kv->key, "Accept-Encoding") && fpm_http_static_accepts(kv->value, coding)) {
			return 1;
		}
	}

	return 0;
}

/* Precompressed sidecars (issue #648), tried in this order: a brotli file, then
 * gzip. A sidecar is found by appending its suffix to the original's path, and
 * it gets the same containment check and the same open() as the original, so a
 * symlink app.css.br that leaves the root is not served. A sidecar that is
 * missing or refused is not an error: the caller serves the original instead. */
static const struct {
	const char *coding;
	const char *suffix;
} fpm_http_static_sidecars[] = {
	{ "br", ".br" },
	{ "gzip", ".gz" },
	{ NULL, NULL }
};

static int fpm_http_static_open_sidecar(const struct fpm_http_static *st, const char *root,
		size_t root_len, const char *candidate, const struct evkeyvalq *in,
		struct stat *stbuf, const char **coding)
{
	unsigned i;

	for (i = 0; fpm_http_static_sidecars[i].coding; i++) {
		char side[MAXPATHLEN], resolved[MAXPATHLEN];
		int fd;

		if (!fpm_http_static_header_accepts(in, fpm_http_static_sidecars[i].coding)) {
			continue;
		}
		if ((size_t) snprintf(side, sizeof(side), "%s%s", candidate, fpm_http_static_sidecars[i].suffix) >= sizeof(side)) {
			continue;
		}
		if (!realpath(side, resolved)) {
			continue;
		}
		if (!fpm_http_static_contained(resolved, root, root_len)) {
			zlog(ZLOG_NOTICE, "[pool %s] http: refused sidecar '%s' outside the document root", st->pool, side);
			continue;
		}
		fd = fpm_http_static_open_regular(resolved, stbuf);
		if (fd >= 0) {
			*coding = fpm_http_static_sidecars[i].coding;
			return fd;
		}
	}

	return -1;
}

/* Range is evaluated only for a GET that has exactly one Range line, and only
 * when If-Range, if present, still matches the representation (RFC 9110
 * 13.1.5, 14.2). Two Range lines would combine into a multi-range set, which
 * is ignored like any other multi-range request. */
static enum fpm_http_static_range fpm_http_static_request_range(const struct evkeyvalq *in,
		off_t size, const char *etag, const char *modified, off_t *first, off_t *last)
{
	struct evkeyval *kv;
	const char *range = NULL, *if_range;
	int n = 0;

	TAILQ_FOREACH(kv, in, next) {
		if (!strcasecmp(kv->key, "Range")) {
			range = kv->value;
			n++;
		}
	}
	if (n != 1) {
		return FPM_HTTP_STATIC_RANGE_IGNORE;
	}
	if_range = evhttp_find_header(in, "If-Range");
	if (if_range && !fpm_http_static_if_range_matches(if_range, etag, modified)) {
		return FPM_HTTP_STATIC_RANGE_IGNORE;
	}

	return fpm_http_static_parse_range(range, size, first, last);
}

/* The headers a precompressed or Cache-Control-configured reply carries. Vary
 * goes on every reply of a precompressed pool, the identity one and the 304
 * too: a shared cache must not store a br body under a key that ignores
 * Accept-Encoding, whichever reply it kept. */
static void fpm_http_static_policy_headers(const struct fpm_http_static *st, struct evkeyvalq *out,
		const char *path)
{
	const char *cache_control = fpm_http_static_cache_control_for(st->cache_control, path);

	if (st->precompressed) {
		evhttp_add_header(out, "Vary", "Accept-Encoding");
	}
	if (cache_control) {
		evhttp_add_header(out, "Cache-Control", cache_control);
	}
}

int fpm_http_static_serve(const struct fpm_http_static *st, struct evhttp_request *req,
		const char *path, size_t path_len, int *script_missing)
{
	char candidate[MAXPATHLEN], resolved[MAXPATHLEN], root_buf[MAXPATHLEN], etag[64], modified[64];
	struct evkeyvalq *out;
	struct stat stbuf, sidebuf;
	const char *type, *root = st->root, *coding = NULL, *reason = "OK";
	enum fpm_http_static_range range = FPM_HTTP_STATIC_RANGE_IGNORE;
	off_t first = 0, last = 0, offset = 0, length;
	size_t root_len;
	int fd, cmd, status = HTTP_OK;

	if (!root && !st->root_unresolved) {
		return 0;
	}
	cmd = evhttp_request_get_command(req);
	if (cmd != EVHTTP_REQ_GET && cmd != EVHTTP_REQ_HEAD) {
		return 0;
	}
	/* Anything that is a PHP script, or has PATH_INFO behind one, is the
	 * worker's business. A directory falls through to the front controller. */
	if (!path_len || path[path_len - 1] == '/' || strstr(path, ".php/")) {
		return 0;
	}
	if (path_len >= 4 && !strcasecmp(path + path_len - 4, ".php")) {
		return 0;
	}
	/* An extension this module has no type for: with known_types_only the
	 * path is not ours at all, and PHP decides what it is. Checked on the
	 * requested path rather than on the resolved one so that the answer does
	 * not depend on where a symlink points.
	 *
	 * Before the dotfile rule below, and that order is the rule: this module
	 * never *serves* a path with a dot-segment in it, but it also does not take
	 * URIs away from PHP that it would not have served anyway. So /.env is
	 * still the application's to route (it was before http.static was turned
	 * on, and enabling a file server must not make URLs disappear), while
	 * /.env.css -- something this module would otherwise have handed over -- is
	 * refused outright. Either way no file whose path contains a dot-segment
	 * ever leaves here. */
	if (st->known_types_only && !fpm_http_static_known_type(path)) {
		return 0;
	}
	if (fpm_http_static_path_has_dotfile(path)) {
		fpm_http_static_refuse(st, req);
		return 1;
	}

	/* Resolved here, after every cheap reject above, so a POST or a .php
	 * request pays no realpath() (issue #638). A root that does not resolve
	 * serves nothing. */
	if (!root) {
		if (!realpath(st->root_unresolved, root_buf)) {
			return 0;
		}
		root = root_buf;
	}
	root_len = strlen(root);

	if ((size_t) snprintf(candidate, sizeof(candidate), "%s%s", root, path) >= sizeof(candidate)) {
		fpm_http_static_refuse(st, req);
		return 1;
	}
	/* realpath() is the only honest containment check: a textual /../ filter
	 * does not catch a symlink pointing outside the document root. One extra
	 * syscall, but a static hit costs no worker at all, so the trade is easy. */
	if (!realpath(candidate, resolved)) {
		if (script_missing) {
			*script_missing = 1;	/* the caller reuses this instead of stat()-ing again */
		}
		return 0;			/* no such file: let PHP produce the 404 */
	}
	if (script_missing) {
		*script_missing = 0;	/* the file is there, whatever open()/fstat() below say about it */
	}
	if (!fpm_http_static_contained(resolved, root, root_len)) {
		zlog(ZLOG_NOTICE, "[pool %s] http: refused '%s' outside the document root", st->pool, path);
		fpm_http_static_refuse(st, req);
		return 1;
	}

	/* O_NONBLOCK: open() of a FIFO with no writer waits for one, and the
	 * gateway runs every client on one event loop, so a FIFO in the docroot
	 * froze all of them (issue #1008). The S_ISREG test below runs on the
	 * descriptor, so the check is race-free: a path swapped for a FIFO after
	 * realpath() still opens without waiting, and is then refused. O_NOCTTY
	 * keeps a terminal device from becoming this process's controlling tty.
	 * O_NONBLOCK is not cleared afterwards: open(2) says it has no effect on
	 * regular files, and clearing it would cost two fcntl() calls per hit. */
	fd = open(resolved, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
	if (fd < 0) {
		return 0;
	}
	if (fstat(fd, &stbuf) < 0) {
		close(fd);
		return 0;
	}
	if (!S_ISREG(stbuf.st_mode)) {
		close(fd);
		/* A directory with no index is "nothing to serve" the same way a
		 * missing file is: reuse this fstat() -- already paid for -- to say so,
		 * instead of the caller needing a stat() of its own. Other non-regular
		 * types (sockets, devices, ...) are PHP's business, not ours. */
		if (script_missing && S_ISDIR(stbuf.st_mode)) {
			*script_missing = 1;
		}
		return 0;
	}

	/* The sidecar replaces the original for this reply, and it brings its own
	 * validators: the ETag below carries the coding, because two
	 * representations may not share one strong ETag (RFC 9110 8.8.3). The
	 * Content-Type stays the original's, taken from `resolved` above. */
	if (st->precompressed) {
		int sfd = fpm_http_static_open_sidecar(st, root, root_len, candidate,
			evhttp_request_get_input_headers(req), &sidebuf, &coding);

		if (sfd >= 0) {
			close(fd);
			fd = sfd;
			stbuf = sidebuf;
		}
	}

	type = fpm_http_static_content_type(resolved);

	snprintf(etag, sizeof(etag), "\"%llx-%llx%s%s\"",
		(unsigned long long) stbuf.st_mtime, (unsigned long long) stbuf.st_size,
		coding ? "-" : "", coding ? coding : "");
	fpm_http_static_http_date(stbuf.st_mtime, modified, sizeof(modified));

	out = evhttp_request_get_output_headers(req);
	if (fpm_http_static_not_modified(req, etag, modified)) {
		close(fd);
		/* A 304 repeats the validators and nothing else, so that the client can
		 * refresh what it has stored without a body. Cache-Control and Vary
		 * are the exception: they describe the response a 200 would have had. */
		evhttp_add_header(out, "ETag", etag);
		if (modified[0]) {
			evhttp_add_header(out, "Last-Modified", modified);
		}
		fpm_http_static_policy_headers(st, out, path);
		fpm_http_static_log(st, 304, 0);
		evhttp_send_reply(req, 304, "Not Modified", NULL);
		return 1;
	}

	evhttp_add_header(out, "Content-Type", type);
	evhttp_add_header(out, "ETag", etag);
	if (modified[0]) {
		evhttp_add_header(out, "Last-Modified", modified);
	}
	fpm_http_static_policy_headers(st, out, path);
	if (st->ranges) {
		evhttp_add_header(out, "Accept-Ranges", "bytes");
	}
	if (coding) {
		evhttp_add_header(out, "Content-Encoding", coding);
	}

	/* HEAD ignores Range (RFC 9110 14.2), so only a GET reaches the next block. */
	if (cmd == EVHTTP_REQ_GET && st->ranges) {
		range = fpm_http_static_request_range(evhttp_request_get_input_headers(req),
			stbuf.st_size, etag, modified, &first, &last);
	}
	if (range == FPM_HTTP_STATIC_RANGE_UNSATISFIABLE) {
		char crange[64];

		close(fd);
		snprintf(crange, sizeof(crange), "bytes */%lld", (long long) stbuf.st_size);
		evhttp_add_header(out, "Content-Range", crange);
		fpm_http_static_log(st, FPM_HTTP_STATIC_NOT_SATISFIABLE, 0);
		evhttp_send_reply(req, FPM_HTTP_STATIC_NOT_SATISFIABLE, "Range Not Satisfiable", NULL);
		return 1;
	}
	if (range == FPM_HTTP_STATIC_RANGE_PARTIAL) {
		char crange[96];

		snprintf(crange, sizeof(crange), "bytes %lld-%lld/%lld", (long long) first, (long long) last,
			(long long) stbuf.st_size);
		evhttp_add_header(out, "Content-Range", crange);
		offset = first;
		length = last - first + 1;
		status = FPM_HTTP_STATIC_PARTIAL;
		reason = "Partial Content";
	} else {
		length = stbuf.st_size;
	}

	if (cmd == EVHTTP_REQ_HEAD) {
		char len[32];

		close(fd);
		snprintf(len, sizeof(len), "%llu", (unsigned long long) stbuf.st_size);
		evhttp_add_header(out, "Content-Length", len);
		/* HEAD sends no body; log 0 bytes, as nginx' $body_bytes_sent would */
		fpm_http_static_log(st, HTTP_OK, 0);
		evhttp_send_reply(req, HTTP_OK, "OK", NULL);
		return 1;
	}

	/* An empty file has no bytes to send. Measured on the test box: a GET of a
	 * zero-length file reached the 502 below through evbuffer_add_file(), so
	 * the empty body is sent without it (fpmng-http-direct-static-range.phpt). */
	if (length == 0) {
		close(fd);
		fpm_http_static_log(st, status, 0);
		evhttp_send_reply(req, status, reason, NULL);
		return 1;
	}

	/* evbuffer_add_file takes ownership of fd and uses sendfile/mmap where it
	 * can, so the bytes never pass through our address space. */
	{
		struct evbuffer *body = evbuffer_new();

		/* A failed evbuffer_add_file() does not own fd: in libevent 2.1.12
		 * (buffer.c, evbuffer_add_file) a failed evbuffer_file_segment_new()
		 * returns -1 without closing it, and a failed
		 * evbuffer_add_file_segment() keeps the segment (and fd) referenced
		 * by nothing. So close it here on every failure. */
		if (!body || evbuffer_add_file(body, fd, offset, length) < 0) {
			close(fd);
			if (body) {
				evbuffer_free(body);
			}
			fpm_http_static_log(st, FPM_HTTP_STATIC_BAD_GATEWAY, 0);
			evhttp_send_error(req, FPM_HTTP_STATIC_BAD_GATEWAY, "Bad Gateway");
			return 1;
		}
		fpm_http_static_log(st, status, (size_t) length);
		evhttp_send_reply(req, status, reason, body);
		evbuffer_free(body);
	}

	return 1;
}

#endif
