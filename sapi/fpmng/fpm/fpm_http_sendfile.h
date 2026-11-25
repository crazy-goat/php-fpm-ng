/* X-Accel-Redirect and X-Sendfile from an upstream reply (issue #649).
 *
 * A FastCGI worker that has checked access to a file can hand the transfer to
 * the gateway: it answers with a header naming the file, and the gateway
 * serves that file itself. The worker is then free after its short reply
 * instead of holding a process for the whole download.
 *
 * Only http.sendfile_root is searched. The gateway resolves the name under that
 * root with the containment rules of fpm_http_static.c, and any name that
 * fails them is answered 404. The redirect header itself never reaches the
 * client. With the directive unset the header is not acted on: it is forwarded
 * like any other header.
 *
 * A served file keeps the upstream's headers, except the ones that describe the
 * worker's body or the hop to the gateway (see fpm_http_sendfile_reply()). The
 * body is read in windows, so the gateway holds one window per reply, not the
 * file.
 */

#ifndef FPM_HTTP_SENDFILE_H
#define FPM_HTTP_SENDFILE_H 1

#include <stddef.h>
#include <sys/types.h>

struct _fpm_http_conn;

/* The redirect header an upstream sent, kept until the reply is decided, and
 * then the state of the body while the gateway sends it. Owned by the request:
 * fpm_http_conn_free() frees it, and it closes the file. */
struct fpm_http_sendfile_s {
	char *path; /* the value, trailing blanks dropped */
	const char *header; /* "X-Accel-Redirect" or "X-Sendfile", for the log */
	int absolute; /* 1 for X-Sendfile (a filesystem path), 0 for X-Accel-Redirect (a path below the root) */
	int fd; /* the file while its body is sent, -1 otherwise */
	int pumping; /* 1 from the first window until the last one has drained; fpm_http_finish() waits for it */
	off_t offset; /* where the next window is read from: the bytes of the body read so far */
	off_t left; /* bytes of the body not read yet */
};

/* Called by fpm_http_start_reply() for every upstream header line when
 * http.sendfile_root is set. Returns 1 when the header was taken: the caller
 * must then not copy it to the client. Returns 0 for any other header. Returns
 * -1 when the copy failed (out of memory): the caller answers 502, because
 * forwarding the upstream body without the redirect would serve the wrong
 * content. */
int fpm_http_sendfile_note(struct _fpm_http_conn *c, const char *key, const char *value, size_t vlen);

/* Called by fpm_http_start_reply() once the upstream's header block is read and
 * no 502 is due, instead of starting the upstream's reply. Sends the file, or a
 * 404 when the name is refused. The reply is ended by fpm_http_finish() as for
 * any other reply, after the last window of a file has drained. */
void fpm_http_sendfile_reply(struct _fpm_http_conn *c);

void fpm_http_sendfile_free(struct fpm_http_sendfile_s *sf);

/* Configuration check, called from fpm_http_validate_pool(). Returns 0 when
 * root is NULL (the directive is unset) or is an existing directory given as an
 * absolute path other than "/", -1 after logging why not. */
int fpm_http_sendfile_validate(const char *pool, const char *root);

#endif
