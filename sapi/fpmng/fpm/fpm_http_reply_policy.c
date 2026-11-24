/* fpm-ng: response headers and error pages of a gateway (issue #650).
 * See fpm_http_reply_policy.h. */

#include "fpm_config.h"

#include "fpm_http_reply_policy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_http_static.h"
#include "zlog.h"

#define FPM_HTTP_RESPONSE_HEADER_VALUE_MAX 4096

/* The page keys that a gateway can serve. 413 is refused with a reason of its
 * own, see fpm_http_reply_policy_check_page_key(). */
static const int fpm_http_error_page_codes[FPM_HTTP_ERROR_PAGE_COUNT] = { 404, 502, 503 };

/* Names the gateway sets itself or that describe the message framing. A
 * configured copy would either be dropped, be overwritten, or break the
 * reply: the gateway sets Content-Type and Content-Length of every page,
 * Date and Location come from libevent and from the redirects, and the
 * redirect headers are acted on by the gateway (issue #649). */
static const char *const fpm_http_response_header_owned[] = {
	"Content-Length", "Transfer-Encoding", "Connection", "Keep-Alive", "Upgrade", "TE", "Trailer",
	"Content-Type", "Content-Encoding", "Content-Range", "Date", "Location", "Status",
	"X-Request-Id", "X-Accel-Redirect", "X-Sendfile",
	/* The gateway adds these itself on replies it writes, after the configured
	 * set: Retry-After on every 503 (fpm_http_reply_headers()), Expires and
	 * Cache-Control on ping and ready, ETag and Last-Modified on a static file.
	 * Accept-Ranges is stripped from an X-Accel reply (fpm_http_sendfile_strip()),
	 * and Set-Cookie would drop every cookie of the application, as the
	 * configured header replaces the application's of the same name. Refusing
	 * them is simpler than a win-or-lose rule per site, and keeps "the
	 * configured value is on the reply exactly once" true. Vary goes with
	 * them: the gateway sets it on a negotiated static file. */
	"Retry-After", "Expires", "Cache-Control", "ETag", "Last-Modified", "Accept-Ranges", "Set-Cookie", "Vary", NULL
};

static const struct fpm_http_reply_policy_s *fpm_http_reply_policy_current;

/* RFC 9110 5.6.2: a field name is a token. */
static int fpm_http_reply_token_char(unsigned char ch)
{
	return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		   (ch && strchr("!#$%&'*+-.^_`|~", ch));
}

int fpm_http_reply_policy_check_header(const char *name, const char *value, size_t value_len, const char **why)
{
	const unsigned char *p;
	unsigned i;

	if (!*name) {
		*why = "the header name is empty";
		return -1;
	}
	for (p = (const unsigned char *) name; *p; p++) {
		if (!fpm_http_reply_token_char(*p)) {
			*why = "the header name may contain only letters, digits and the characters ! # $ % & ' * + - . ^ _ ` | ~";
			return -1;
		}
	}
	for (i = 0; fpm_http_response_header_owned[i]; i++) {
		if (!strcasecmp(name, fpm_http_response_header_owned[i])) {
			*why = "the gateway sets this header itself; it cannot be configured";
			return -1;
		}
	}
	if (!strncasecmp(name, "Proxy-", 6) || !strncasecmp(name, "X-Fpmng-", 8)) {
		*why = "the Proxy- and X-Fpmng- headers belong to the gateway; they cannot be configured";
		return -1;
	}
	if (value_len == 0) {
		*why = "the value is empty; give a header value";
		return -1;
	}
	if (value_len != strlen(value)) {
		*why = "the value contains a NUL byte";
		return -1;
	}
	if (value_len > FPM_HTTP_RESPONSE_HEADER_VALUE_MAX) {
		*why = "the value is longer than 4096 bytes";
		return -1;
	}
	for (p = (const unsigned char *) value; *p; p++) {
		if (*p == '\r' || *p == '\n') {
			*why = "the value contains a CR or LF; a header value is one line";
			return -1;
		}
		if ((*p < 0x20 && *p != '\t') || *p == 0x7f) {
			*why = "the value contains a control character";
			return -1;
		}
	}
	return 0;
}

int fpm_http_reply_policy_check_page_key(const char *key, int *code, const char **why)
{
	unsigned i;

	if (!strcmp(key, "413")) {
		*why = "413 cannot have a page: libevent writes the 413 for a body over http.max_body before the gateway "
			   "callback runs (docs/gateway.md, \"Why 413 is not a key\"). Use 404, 502 or 503";
		return -1;
	}
	for (i = 0; i < FPM_HTTP_ERROR_PAGE_COUNT; i++) {
		char buf[8];

		snprintf(buf, sizeof(buf), "%d", fpm_http_error_page_codes[i]);
		if (!strcmp(key, buf)) {
			*code = fpm_http_error_page_codes[i];
			return 0;
		}
	}
	*why = "the key must be 404, 502 or 503";
	return -1;
}

int fpm_http_reply_policy_check_page_path(const char *path, const char **why)
{
	if (*path != '/') {
		*why = "the path must be absolute";
		return -1;
	}
	return 0;
}

static const char *fpm_http_error_page_type(const char *path)
{
	const char *type = fpm_http_static_content_type(path);

	/* An unknown extension gives octet-stream. A browser would download such a
	 * page instead of showing it, so the extension is refused at -t. */
	return strcmp(type, "application/octet-stream") ? type : NULL;
}

/* Reads one page file. The file is opened with O_NONBLOCK, so a FIFO named by
 * mistake fails the S_ISREG test instead of blocking the master. */
static int fpm_http_error_page_read(const char *pool, int code, const char *path, struct fpm_http_error_page_s *pg)
{
	struct stat st;
	const char *type = fpm_http_error_page_type(path);
	char *body;
	ssize_t n;
	int fd;

	if (!type) {
		zlog(ZLOG_ERROR, "[pool %s] http.error_page[%d] '%s': the file extension has no known content type "
						 "(use .html, .htm, .json, .txt, .xml, .svg ...)",
				pool, code, path);
		return -1;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.error_page[%d] '%s': cannot open the file: %s", pool, code, path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		zlog(ZLOG_ERROR, "[pool %s] http.error_page[%d] '%s': not a regular file", pool, code, path);
		close(fd);
		return -1;
	}
	if (st.st_size > FPM_HTTP_ERROR_PAGE_MAX) {
		zlog(ZLOG_ERROR, "[pool %s] http.error_page[%d] '%s': the file is %lld bytes, the limit is %d",
				pool, code, path, (long long) st.st_size, FPM_HTTP_ERROR_PAGE_MAX);
		close(fd);
		return -1;
	}
	if (st.st_size == 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.error_page[%d] '%s': the file is empty", pool, code, path);
		close(fd);
		return -1;
	}
	body = malloc((size_t) st.st_size);
	if (!body) {
		zlog(ZLOG_ERROR, "[pool %s] http.error_page[%d] '%s': out of memory", pool, code, path);
		close(fd);
		return -1;
	}
	/* One read of the whole size: a short read means the file changed under the
	 * read, and the build is refused rather than a truncated page being kept. */
	n = read(fd, body, (size_t) st.st_size);
	close(fd);
	if (n != (ssize_t) st.st_size) {
		zlog(ZLOG_ERROR, "[pool %s] http.error_page[%d] '%s': cannot read the whole file", pool, code, path);
		free(body);
		return -1;
	}
	pg->type = type;
	pg->body = body;
	pg->len = (size_t) n;
	return 0;
}

int fpm_http_reply_policy_build(struct fpm_worker_pool_s *wp, struct fpm_http_reply_policy_s *out)
{
	const struct key_value_s *kv;
	const char *pool = wp->config->name;
	unsigned n = 0, i;

	memset(out, 0, sizeof(*out));
	for (i = 0; i < FPM_HTTP_ERROR_PAGE_COUNT; i++) {
		out->pages[i].code = fpm_http_error_page_codes[i];
	}
	for (kv = wp->config->http_response_headers; kv; kv = kv->next) {
		n++;
	}
	if (n) {
		out->headers = calloc(n, sizeof(*out->headers));
		if (!out->headers) {
			zlog(ZLOG_ERROR, "[pool %s] http.response_header: out of memory", pool);
			return -1;
		}
		/* fpm_conf_set_array() prepends, so the list is the file in reverse.
		 * Filled from the end, the headers go out in the order of the file. */
		for (kv = wp->config->http_response_headers, i = n; kv; kv = kv->next) {
			i--;
			out->headers[i].name = strdup(kv->key);
			out->headers[i].value = strdup(kv->value);
			out->nheaders = n;
			if (!out->headers[i].name || !out->headers[i].value) {
				zlog(ZLOG_ERROR, "[pool %s] http.response_header: out of memory", pool);
				fpm_http_reply_policy_free(out);
				return -1;
			}
		}
	}
	for (kv = wp->config->http_error_pages; kv; kv = kv->next) {
		const char *why = "";
		int code = 0;

		if (fpm_http_reply_policy_check_page_key(kv->key, &code, &why) < 0) {
			zlog(ZLOG_ERROR, "[pool %s] http.error_page[%s]: %s", pool, kv->key, why);
			fpm_http_reply_policy_free(out);
			return -1;
		}
		for (i = 0; i < FPM_HTTP_ERROR_PAGE_COUNT; i++) {
			if (out->pages[i].code == code && fpm_http_error_page_read(pool, code, kv->value, &out->pages[i]) < 0) {
				fpm_http_reply_policy_free(out);
				return -1;
			}
		}
	}
	return 0;
}

void fpm_http_reply_policy_free(struct fpm_http_reply_policy_s *p)
{
	unsigned i;

	for (i = 0; i < p->nheaders; i++) {
		free(p->headers[i].name);
		free(p->headers[i].value);
	}
	free(p->headers);
	for (i = 0; i < FPM_HTTP_ERROR_PAGE_COUNT; i++) {
		free(p->pages[i].body);
	}
	memset(p, 0, sizeof(*p));
}

void fpm_http_reply_policy_install(const struct fpm_http_reply_policy_s *p)
{
	fpm_http_reply_policy_current = p;
}

const struct fpm_http_reply_policy_s *fpm_http_reply_policy_installed(void)
{
	return fpm_http_reply_policy_current;
}

const struct fpm_http_error_page_s *fpm_http_reply_policy_page(const struct fpm_http_reply_policy_s *p, int code)
{
	unsigned i;

	if (!p) {
		return NULL;
	}
	for (i = 0; i < FPM_HTTP_ERROR_PAGE_COUNT; i++) {
		if (p->pages[i].code == code && p->pages[i].len) {
			return &p->pages[i];
		}
	}
	return NULL;
}

void fpm_http_reply_headers_add(const struct fpm_http_reply_policy_s *p, struct evkeyvalq *out)
{
	unsigned i;

	if (!p) {
		return;
	}
	for (i = 0; i < p->nheaders; i++) {
		evhttp_add_header(out, p->headers[i].name, p->headers[i].value);
	}
}

int fpm_http_reply_header_owned(const struct fpm_http_reply_policy_s *p, const char *name)
{
	unsigned i;

	if (!p) {
		return 0;
	}
	for (i = 0; i < p->nheaders; i++) {
		if (!strcasecmp(p->headers[i].name, name)) {
			return 1;
		}
	}
	return 0;
}

/* Nothing to free: the page belongs to the policy, which lives as long as the
 * gateway process. */
static void fpm_http_error_page_unref(const void *data, size_t len, void *arg)
{
	(void) data;
	(void) len;
	(void) arg;
}

int fpm_http_error_page_add(const struct fpm_http_error_page_s *pg, struct evbuffer *buf)
{
	return evbuffer_add_reference(buf, pg->body, pg->len, fpm_http_error_page_unref, NULL);
}

size_t fpm_http_error_page_put(const struct fpm_http_error_page_s *pg, struct evhttp_request *req)
{
	struct evbuffer *buf = evhttp_request_get_output_buffer(req);

	evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", pg->type);
	/* A HEAD reply gets no body, as in fpm_http_error_prepare(). */
	if (evhttp_request_get_command(req) == EVHTTP_REQ_HEAD) {
		return 0;
	}
	if (fpm_http_error_page_add(pg, buf) < 0) {
		return 0;
	}
	return evbuffer_get_length(buf);
}
