/* fpm-ng: reload the HTTP gateway's TLS certificate/key without restarting
 * any gateway process (task 040). See fpm_http_tls_reload.c for the design
 * (master-side mtime timer + validate, double-buffered shared memory,
 * per-child adoption timer) and task 040 (done; see docs/task-archive.md)
 * for the reasoning behind each choice.
 *
 * Layered on top of fpm_http_tls.h, not a replacement for it: fpm_http_tls_s,
 * fpm_http_tls_validate(), fpm_http_tls_load() and fpm_http_tls_ctx_new() are
 * reused as-is, unmodified.
 */

#ifndef FPM_HTTP_TLS_RELOAD_H
#define FPM_HTTP_TLS_RELOAD_H 1

#include "fpm_config.h"

#ifdef HAVE_FPM_HTTP_TLS

#include <openssl/ssl.h>
#include <event2/event.h>
#include <event2/http.h>

#include "fpm_http_tls.h"

/* http.tls_reload_check, when the directive is not set at all (as opposed to
 * set to 0, which means "off"). Matches the "default e.g. 5" left open by
 * task 040's Decision section: this is a config/perf choice, not a
 * correctness one -- fast enough that a renewed certificate is picked up
 * quickly, slow enough that N gateway processes stat()ing two files does
 * not show up as load. */
#define FPM_HTTP_TLS_RELOAD_CHECK_DEFAULT 5

/* Bound on a single cert-chain or key PEM this mechanism will publish, sized
 * generously for a real fullchain.pem (leaf + a couple of intermediates,
 * typically a few KB) rather than measured against a specific deployment.
 * A candidate exceeding this is rejected exactly like any other invalid
 * candidate: logged, not installed, the certificate already in use keeps
 * serving (see fpm_http_tls_reload_master_init() and the master tick in
 * fpm_http_tls_reload.c). */
#define FPM_HTTP_TLS_RELOAD_MAX_CERT (64 * 1024)
#define FPM_HTTP_TLS_RELOAD_MAX_KEY  (16 * 1024)

struct fpm_http_tls_reload_s;

/* Called once per TLS pool in the master, right after fpm_http_tls_load()
 * (fpm_http.c, before the first gateway child is forked): allocates the
 * shared-memory double buffer, publishes `initial` (the bytes
 * fpm_http_tls_load() just validated) as generation 0, and -- when
 * check_interval_sec > 0 -- arms the master's own mtime-check timer on
 * cert_path/key_path. Returns NULL (logged) on shm allocation failure or
 * when `initial` is already bigger than the reload buffer; either way the
 * pool keeps working exactly as it did before this task, just without the
 * ability to reload without a restart. */
struct fpm_http_tls_reload_s *fpm_http_tls_reload_master_init(const char *pool,
	const char *cert_path, const char *key_path, const char *min_version,
	struct fpm_http_tls_s *initial, int check_interval_sec);

/* Called once per gateway child, after fpm_http_tls_ctx_new() has built the
 * child's own SSL_CTX and evhttp_set_bevcb() has installed it
 * (fpm_http_gateway_run()): arms this child's own generation-watch timer on
 * its own event_base. On a later generation change it rebuilds the SSL_CTX
 * from the newly published bytes, calls evhttp_set_bevcb() again on `http`
 * to point future connections at it, frees the old context, and writes the
 * new one through `ctx_slot` -- `*ctx_slot` must be the same pointer
 * (gw->tls_ctx) fpm_http_tls_ctx_new() originally filled in, since this is
 * also what fpm_http_tls_bevcb() was handed as its `arg`. A no-op when
 * `reload` is NULL or its check interval is 0 (http.tls_reload_check off,
 * or master-side setup failed). */
void fpm_http_tls_reload_child_init(struct fpm_http_tls_reload_s *reload,
	struct event_base *base, struct evhttp *http, SSL_CTX **ctx_slot,
	struct bufferevent *(*bevcb)(struct event_base *, void *), void *bevcb_arg);

/* Master-only: releases the shared-memory double buffer and the struct
 * itself. Never called by a gateway child -- children exit() rather than
 * unwind, same as the rest of struct fpm_http_gateway_s. */
void fpm_http_tls_reload_free(struct fpm_http_tls_reload_s *reload);

#endif /* HAVE_FPM_HTTP_TLS */

#endif
