/* fpm-ng: the request side of the HTTP-direct transport, shared by both
 * executors. See fpm_http_direct_request.c. */

#ifndef FPM_HTTP_DIRECT_REQUEST_H
#define FPM_HTTP_DIRECT_REQUEST_H 1

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#include <event2/http.h>

struct fpm_worker_pool_s;

/* evhttp's limit on the whole request header block
 * (evhttp_set_max_headers_size), and therefore the ceiling on a response
 * header block as well. 64 KiB is the number HTTP-direct has always used;
 * the HTTP gateway adopted it rather than picking its own, so a request that
 * one transport refuses is refused by the other (issue #117). It is already
 * generous next to what the servers this replaces accept: Apache's
 * LimitRequestFieldSize is 8190 bytes per line with LimitRequestFields 100,
 * nginx' large_client_header_buffers is 4 x 8k.
 *
 * Not DIRECT_: both transports set it (fpm_http_direct.c,
 * fpm_http_direct_worker.c, fpm_http.c). */
#define FPM_HTTP_HEADERS_MAX (64 * 1024)
/* Longest header name turned into an HTTP_* CGI key, and the reason the key
 * buffer is a few hundred bytes instead of FPM_HTTP_HEADERS_MAX + 6: that
 * constant bounds the header *block*, so sizing a per-iteration stack array
 * from it cost 64 KB of stack per header. A name this long is already
 * pathological — Apache rejects a whole header line above 8190 bytes — and a
 * request carrying one is refused, not served with the header dropped.
 *
 * Not DIRECT_ either: the HTTP gateway (fpm_http.c) enforces the same bound
 * the same way, so a request header name means one thing on both transports
 * (issue #115). */
#define FPM_HTTP_HEADER_NAME_MAX 1024

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
	/* NULL-terminated list of http.* directives this executor accepts on top
	 * of the shared allow-list below, or NULL for none. Kept as data rather
	 * than a branch on the executor so the allow-list stays one loop. */
	const char *const *extra_directives;
};

/* Pool directives neither executor can honour. Shared as a macro rather than
 * an array so a list can extend it (the worker adds the per-request deadlines
 * and the per-request observability) without holding a second copy of the
 * common entries.
 *
 * pm.status_listen is rejected for a reason that changed shape in #273 without
 * going away. Under its upstream meaning it asked for a second listening socket
 * served by a second FastCGI pool, and a direct child owns exactly one listener
 * -- the pool's. Under the new meaning it names where this pool's operator
 * endpoint binds (fpm_operator_endpoint.h), and http-direct has not moved its
 * status page there: the page is rendered inside the child that answers and
 * reports per-child rows nothing outside the pool can produce yet
 * (fpm_pool_type.h, .status_on_own_listener). So the directive still has
 * nothing to name here, and saying so is better than accepting an address and
 * binding nothing. #275 moves the page and removes this entry.
 *
 * pm.metrics_listen is deliberately NOT rejected: the metrics path is a new
 * directive with no second meaning, and it does go to the operator listener.
 *
 * pm.status_path on the pool's own listener is supported by the classic
 * executor (issue #59) and is the subject of #275. */
#define FPM_HTTP_DIRECT_REJECTS_COMMON \
	"pm.status_listen", "fiber.", "supervisor.", "cron."

/* CGI values that come from the pool rather than from the request. */
struct fpm_http_direct_env_source {
	const char *script;		/* resolved SCRIPT_FILENAME */
	const char *root;		/* DOCUMENT_ROOT */
	const char *front_controller;	/* SCRIPT_NAME / PHP_SELF */
	const char *server_addr;
	const char *server_port;
	const char *server_software;
	/* issue #55: the pool terminates TLS, so REQUEST_SCHEME is https and
	 * HTTPS is on for every request it serves. A pool-wide property, not a
	 * per-connection one: a direct pool has no second plain listener. */
	bool tls;
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
/* Charges one emitted response header line against FPM_HTTP_HEADERS_MAX
 * and returns false when it does not fit, leaving *total unchanged. */
bool fpm_http_direct_header_charge(size_t *total, const char *name, size_t value_len);
bool fpm_http_direct_status_final(long status);
bool fpm_http_direct_status_bodyless(struct evhttp_request *http, int status);

#endif
