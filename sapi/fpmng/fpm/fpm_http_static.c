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
#include <sys/stat.h>
#include <sys/types.h>

#include <event2/buffer.h>
#include <event2/http.h>

#include "zlog.h"

#ifndef MAXPATHLEN
#define MAXPATHLEN PATH_MAX
#endif

#define FPM_HTTP_STATIC_BAD_GATEWAY 502 /* libevent has no constant for it */

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

int fpm_http_static_serve(const struct fpm_http_static *st, struct evhttp_request *req,
		const char *path, size_t path_len, int *script_missing)
{
	char candidate[MAXPATHLEN], resolved[MAXPATHLEN], etag[64], modified[64];
	struct evkeyvalq *out;
	struct stat stbuf;
	const char *type;
	size_t root_len;
	int fd, cmd;

	if (!st->root) {
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

	root_len = strlen(st->root);

	if ((size_t) snprintf(candidate, sizeof(candidate), "%s%s", st->root, path) >= sizeof(candidate)) {
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
	if (strncmp(resolved, st->root, root_len) != 0 || (resolved[root_len] && resolved[root_len] != '/')) {
		zlog(ZLOG_NOTICE, "[pool %s] http: refused '%s' outside the document root", st->pool, path);
		fpm_http_static_refuse(st, req);
		return 1;
	}

	fd = open(resolved, O_RDONLY | O_CLOEXEC);
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

	type = fpm_http_static_content_type(resolved);

	snprintf(etag, sizeof(etag), "\"%llx-%llx\"",
		(unsigned long long) stbuf.st_mtime, (unsigned long long) stbuf.st_size);
	fpm_http_static_http_date(stbuf.st_mtime, modified, sizeof(modified));

	out = evhttp_request_get_output_headers(req);
	if (fpm_http_static_not_modified(req, etag, modified)) {
		close(fd);
		/* A 304 repeats the validators and nothing else, so that the client can
		 * refresh what it has stored without a body. */
		evhttp_add_header(out, "ETag", etag);
		if (modified[0]) {
			evhttp_add_header(out, "Last-Modified", modified);
		}
		fpm_http_static_log(st, 304, 0);
		evhttp_send_reply(req, 304, "Not Modified", NULL);
		return 1;
	}

	evhttp_add_header(out, "Content-Type", type);
	evhttp_add_header(out, "ETag", etag);
	if (modified[0]) {
		evhttp_add_header(out, "Last-Modified", modified);
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

	/* evbuffer_add_file takes ownership of fd and uses sendfile/mmap where it
	 * can, so the bytes never pass through our address space. */
	{
		struct evbuffer *body = evbuffer_new();

		if (!body || evbuffer_add_file(body, fd, 0, stbuf.st_size) < 0) {
			if (body) {
				evbuffer_free(body);
			} else {
				close(fd);
			}
			fpm_http_static_log(st, FPM_HTTP_STATIC_BAD_GATEWAY, 0);
			evhttp_send_error(req, FPM_HTTP_STATIC_BAD_GATEWAY, "Bad Gateway");
			return 1;
		}
		fpm_http_static_log(st, HTTP_OK, (size_t) stbuf.st_size);
		evhttp_send_reply(req, HTTP_OK, "OK", body);
		evbuffer_free(body);
	}

	return 1;
}

#endif
