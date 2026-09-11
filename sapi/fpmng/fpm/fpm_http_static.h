/* Serving files from disk without occupying a worker.
 *
 * One implementation, two callers: the `http` gateway (fpm_http.c), which has
 * served static files since task 018, and `pool.type = http-direct`
 * (fpm_http_direct.c), which gained the same ability in issue #58. Extracted
 * rather than reimplemented on purpose -- the containment check, the dotfile
 * rule and the conditional-request handling are the parts that have to be
 * right, and a second copy of them is a second place to get them wrong.
 *
 * The caller supplies the resolved document root and, optionally, a callback
 * for its access log: the gateway writes one, a direct pool has no access.log
 * at all (it is in FPM_HTTP_DIRECT_REJECTS_COMMON), so the hook is how the
 * difference stays out of this file.
 */

#ifndef FPM_HTTP_STATIC_H
#define FPM_HTTP_STATIC_H 1

#include <sys/types.h>
#include <event2/http.h>

struct fpm_http_static {
	const char *pool;	/* for zlog() only */
	/* Already resolved with realpath(), by whoever owns it: the gateway does
	 * it once per process, a direct pool gets it from
	 * fpm_http_direct_resolve_script(). NULL disables serving entirely. */
	const char *root;
	/* When set, only the extensions this module has a MIME type for are served
	 * and anything else falls through to PHP -- the "allowed file classes"
	 * rule issue #58 asks a direct pool to state explicitly. The gateway
	 * leaves it 0 and keeps serving unknown extensions as
	 * application/octet-stream, which is what it has always done. */
	int known_types_only;
	/* Called just before each reply this module sends, or not at all. */
	void (*log)(void *ctx, int status, size_t bytes);
	void *log_ctx;
};

/* Returns 1 when the request has been answered and the caller must do nothing
 * more with it, 0 when this path is not something to serve from disk and the
 * caller should carry on to PHP.
 *
 * `script_missing`, when not NULL, is set to 1 if the path resolves to nothing
 * (or to a directory) and 0 if a file is there -- so a caller that would
 * otherwise stat() the same path again does not have to.
 */
int fpm_http_static_serve(const struct fpm_http_static *st, struct evhttp_request *req,
	const char *path, size_t path_len, int *script_missing);

/* The request path, percent-decoded, with the traversal and embedded-NUL
 * checks every caller has to make before a path from the wire may touch the
 * filesystem. Returns a malloc()ed string the caller frees, or NULL when the
 * request has no usable path -- which the caller must treat as "not mine",
 * leaving PHP to produce the error. */
char *fpm_http_static_decode_path(struct evhttp_request *req, size_t *len);

/* The extension-to-MIME table this module serves with; exposed because the
 * gateway names it in its own responses too. Never NULL: unknown extensions
 * are application/octet-stream. */
const char *fpm_http_static_content_type(const char *path);

#endif
