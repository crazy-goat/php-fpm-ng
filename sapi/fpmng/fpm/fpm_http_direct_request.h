/* fpm-ng: the request side of the HTTP-direct transport, shared by both
 * executors. See fpm_http_direct_request.c. */

#ifndef FPM_HTTP_DIRECT_REQUEST_H
#define FPM_HTTP_DIRECT_REQUEST_H 1

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#include <event2/http.h>

struct fpm_worker_pool_s;

/* evhttp's limit on the whole header block (evhttp_set_max_headers_size), and
 * therefore the ceiling on a response header block as well. */
#define FPM_HTTP_DIRECT_HEADERS_MAX (64 * 1024)
/* Longest header name turned into an HTTP_* CGI key, and the reason the key
 * buffer is a few hundred bytes instead of FPM_HTTP_DIRECT_HEADERS_MAX + 6:
 * that constant bounds the header *block*, so sizing a per-iteration stack
 * array from it cost 64 KB of stack per header. A name this long is already
 * pathological — Apache rejects a whole header line above 8190 bytes — and a
 * request carrying one is refused, not served with the header dropped. */
#define FPM_HTTP_DIRECT_HEADER_NAME_MAX 1024

/* Wording the two executors do not share. The checks below are identical; the
 * text an operator reads must still name the mode they configured, and the
 * .phpt suite matches on these strings (fpmng-http-direct-config.phpt,
 * fpmng-config-rejected-directives.phpt). */
struct fpm_http_direct_labels {
	const char *subject;		/* "http-direct" | "pool.executor = worker" */
	const char *chdir_note;		/* "" | " (here: the worker script)" */
	const char *type_label;		/* what an unsupported http.* is not supported by */
	const char *script_context;	/* log prefix of the script-resolution error */
	const char *script_noun;	/* "front controller" | "the worker script" */
};

/* Pool directives neither executor can honour. Shared as a macro rather than
 * an array so a list can extend it (the worker adds the per-request deadlines)
 * without holding a second copy of the common entries. */
#define FPM_HTTP_DIRECT_REJECTS_COMMON \
	"fiber.", "supervisor.", "cron.", "chroot", "listen.allowed_clients", \
	"pm.status_path", "pm.status_listen", "ping.path", "ping.response", \
	"access.log", "access.format", "access.suppress_path"

/* CGI values that come from the pool rather than from the request. */
struct fpm_http_direct_env_source {
	const char *script;		/* resolved SCRIPT_FILENAME */
	const char *root;		/* DOCUMENT_ROOT */
	const char *front_controller;	/* SCRIPT_NAME / PHP_SELF */
	const char *server_addr;
	const char *server_port;
	const char *server_software;
};

/* Non-zero from the callback aborts the walk; fpm_http_direct_build_env()
 * then returns -1 and the caller answers 400. */
typedef int (*fpm_http_direct_env_cb)(void *ctx, const char *key, const char *value);

int fpm_http_direct_validate_common(struct fpm_worker_pool_s *wp, const struct fpm_http_direct_labels *labels);
int fpm_http_direct_resolve_script(const char *base, const char *front_controller,
	char root[PATH_MAX], char script[PATH_MAX]);
const char *fpm_http_direct_method(enum evhttp_cmd_type command);
bool fpm_http_direct_request_acceptable(struct evhttp_request *http);
int fpm_http_direct_build_env(struct evhttp_request *http, const struct fpm_http_direct_env_source *source,
	fpm_http_direct_env_cb emit, void *ctx);
bool fpm_http_direct_header_dropped(const char *name);
bool fpm_http_direct_header_name_ok(const char *name);
/* Writes `name` into `out` (size bytes, always NUL-terminated) with every byte
 * outside printable US-ASCII escaped as \xNN, truncating rather than growing.
 * Returns `out`, so it can be used inline in a log call. */
const char *fpm_http_direct_header_name_escape(const char *name, char *out, size_t size);
/* Charges one emitted response header line against FPM_HTTP_DIRECT_HEADERS_MAX
 * and returns false when it does not fit, leaving *total unchanged. */
bool fpm_http_direct_header_charge(size_t *total, const char *name, size_t value_len);
bool fpm_http_direct_status_final(long status);
bool fpm_http_direct_status_bodyless(struct evhttp_request *http, int status);

#endif
