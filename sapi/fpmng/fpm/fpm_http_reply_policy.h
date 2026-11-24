/* fpm-ng: response headers and error pages of a gateway (issue #650).
 *
 * Two things an operator of a directly exposed gateway needs and cannot get
 * from the application: a site-wide header such as Strict-Transport-Security
 * on every reply, and a maintenance page for the 404, 502 and 503 that the
 * gateway writes itself.
 *
 *   http.response_header[<Name>] = "<value>"
 *   http.error_page[<404|502|503>] = /absolute/path/to/page.html
 *
 * Both are read by the master. The page files are read at configuration time
 * (and again by `php-fpm-ng -t`), so serving a page needs no file access at
 * request time. The policy reaches every gateway process through fork(). The
 * rules are in docs/gateway.md, "Response headers and error pages". */

#ifndef FPM_HTTP_REPLY_POLICY_H
#define FPM_HTTP_REPLY_POLICY_H 1

#include <stddef.h>

struct evbuffer;
struct evhttp_request;
struct evkeyvalq;
struct fpm_worker_pool_s;

/* A page is at most this many bytes: it is held in the memory of every
 * gateway process, and a maintenance page is small. */
#define FPM_HTTP_ERROR_PAGE_MAX (64 * 1024)
/* The page keys. The count is the length of fpm_http_reply_policy_s.pages. */
#define FPM_HTTP_ERROR_PAGE_COUNT 3

struct fpm_http_response_header_s {
	char *name;
	char *value;
};

struct fpm_http_error_page_s {
	int code; /* 404, 502 or 503 */
	const char *type; /* Content-Type, from the file extension; points into a static table */
	char *body; /* NULL and len == 0 when the page is not configured */
	size_t len;
};

struct fpm_http_reply_policy_s {
	struct fpm_http_response_header_s *headers;
	unsigned nheaders;
	struct fpm_http_error_page_s pages[FPM_HTTP_ERROR_PAGE_COUNT];
};

/* Parse-time checks, called from fpm_conf.c. Each returns 0 when the entry is
 * acceptable, or -1 and a reason in *why (a static string). */
int fpm_http_reply_policy_check_header(const char *name, const char *value, size_t value_len, const char **why);
/* On success *code is the status the key names. */
int fpm_http_reply_policy_check_page_key(const char *key, int *code, const char **why);
int fpm_http_reply_policy_check_page_path(const char *path, const char **why);

/* Builds the policy of a pool from its configuration and reads the page files.
 * Returns 0, or -1 after logging the reason. `out` is zeroed first, so a
 * failed build leaves nothing to free. Run by the master and by `-t`. */
int fpm_http_reply_policy_build(struct fpm_worker_pool_s *wp, struct fpm_http_reply_policy_s *out);
void fpm_http_reply_policy_free(struct fpm_http_reply_policy_s *p);

/* Makes `p` the policy that fpm_http_error_prepare() reads. Called once in a
 * gateway process, after fork(). An http-direct process never calls it, so
 * the shared error and static code finds no policy there. */
void fpm_http_reply_policy_install(const struct fpm_http_reply_policy_s *p);
const struct fpm_http_reply_policy_s *fpm_http_reply_policy_installed(void);

/* The page for `code`, or NULL when there is none (also for p == NULL). */
const struct fpm_http_error_page_s *fpm_http_reply_policy_page(const struct fpm_http_reply_policy_s *p, int code);

/* Adds every configured header to `out`. */
void fpm_http_reply_headers_add(const struct fpm_http_reply_policy_s *p, struct evkeyvalq *out);

/* 1 when `name` is a configured header, compared without case. The copy loop
 * of an upstream reply drops such a header: the configured value wins. */
int fpm_http_reply_header_owned(const struct fpm_http_reply_policy_s *p, const char *name);

/* Adds the body of the page to `buf` by reference (no copy). Returns -1 when
 * the add failed. */
int fpm_http_error_page_add(const struct fpm_http_error_page_s *pg, struct evbuffer *buf);

/* Puts the page on `req`: its Content-Type, and for any method but HEAD its
 * body, by reference (no copy). Returns the length of the output buffer after
 * the add, which is what goes on the wire, so 0 for HEAD or when the add
 * failed (the rule of issue #967). */
size_t fpm_http_error_page_put(const struct fpm_http_error_page_s *pg, struct evhttp_request *req);

#endif /* FPM_HTTP_REPLY_POLICY_H */
