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
 * pm.status_listen and pm.metrics_listen are deliberately NOT here. Since issue
 * #275 both of this type's operator pages are served by the operator endpoint
 * (fpm_operator_endpoint.h) on a listener of its own, so both directives have
 * something to name. The upstream meaning of pm.status_listen -- a second
 * FastCGI socket served by a second pool -- is gone from fpm-ng, and a direct
 * child still owns exactly one request listener; what changed is that the
 * operator page is no longer on it.
 *
 * pm.status_path on the pool's own listener was supported by the classic
 * executor between issues #59 and #275; it is now answered on the operator
 * listener instead, unchanged. The worker executor rejects it still (see
 * fpm_http_direct_worker_rejects) for reasons of its own.
 *
 * worker. is here too (issue #331): worker.max_pending and
 * worker.request_timeout mean something only under pool.executor = worker, so
 * the classic executor rejects the whole namespace like it does fiber.'s. The
 * worker executor's own rejects array (fpm_http_direct_worker_rejects) starts
 * from this same macro and carves the two directives back out via
 * .reject_exceptions on fpm_http_direct_worker's fpm_pool_type_s entry
 * (fpm_pool_type.c) -- the established mechanism for "reject a whole prefix,
 * name the exceptions", see fpm_pool_type.h. */
#define FPM_HTTP_DIRECT_REJECTS_COMMON \
	"fiber.", "supervisor.", "cron.", "worker."

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

/* Client-certificate field formatting shared by fpm_connection_info() on both
 * executors (classic: fpm_http_direct.c; worker: fpm_http_direct_worker.c,
 * issue #335). Compiled in only when the build has OpenSSL (HAVE_FPM_HTTP_TLS,
 * fpm_config.h's signal for that -- see fpm_tls_http.h's comment); the
 * includer is expected to have included fpm_config.h already, the same
 * assumption fpm_tls_http.h makes. <openssl/x509.h> is included here rather
 * than assumed, since not every includer of this header (fpm_http.c, for one)
 * otherwise has a reason to pull it in. */
#ifdef HAVE_FPM_HTTP_TLS
#include <openssl/x509.h>
char *fpm_http_direct_x509_name(X509_NAME *name);
char *fpm_http_direct_x509_time(const ASN1_TIME *t);
char *fpm_http_direct_x509_fingerprint(X509 *cert);
#endif

#endif
