/* fpm-ng: TLS termination for pool.type = http-direct, both executors
 * (issue #55).
 *
 * Deliberately thin. The gateway already terminates TLS and already reloads a
 * certificate without restarting anything, and all of that lives behind two
 * headers that know nothing about gateways: fpm_http_tls.h (validate, load in
 * the master, build an SSL_CTX per process) and fpm_http_tls_reload.h
 * (master-side digest timer over a shared-memory double buffer, per-child
 * adoption timer). Direct pools accept on the same kind of `struct evhttp`,
 * so what was missing was not a TLS implementation but somewhere to keep the
 * per-pool state and the two call sites that hang it off the child's loop.
 * That is all this file is; no TLS logic is duplicated here.
 *
 * The direct transport is simpler than the gateway in the one way that
 * matters here: a direct pool has no second plain listener (there is no
 * http.plain_listen for it), so a pool either speaks TLS on `listen` or it
 * does not, and every connection of a TLS pool is a TLS connection.
 */

#ifndef FPM_HTTP_DIRECT_TLS_H
#define FPM_HTTP_DIRECT_TLS_H 1

#include "fpm_config.h"

#include <stdbool.h>

struct fpm_worker_pool_s;
struct event_base;
struct evhttp;

/* Config validation, in the master, before anything forks. Refuses a
 * half-configured pair, refuses TLS at all in a build without OpenSSL or
 * libevent's OpenSSL glue (naming what is missing rather than serving plain
 * HTTP on a port the operator configured as HTTPS), and otherwise hands
 * cert/key/min_version/sni to fpm_http_tls_validate(), which is the same
 * check the `http` pool type gets. Returns 0 or -1, logging by itself. */
int fpm_http_direct_tls_validate(struct fpm_worker_pool_s *wp);

/* Once per pool, in the master, after validation and BEFORE the first child
 * forks: reads the PEM files into memory and arms the reload machinery, so
 * every child inherits the bytes through fork() and no child ever opens the
 * private key from disk. A no-op for a pool without http.tls_cert. Returns 0
 * or -1 (logged). */
int fpm_http_direct_tls_init_main(struct fpm_worker_pool_s *wp);

/* Once per child, after evhttp_new() and BEFORE the listening socket is
 * attached: builds this process's own SSL_CTX from the currently published
 * generation and installs the bevcb, so the first connection this child ever
 * accepts is already TLS and already on the newest certificate (the
 * respawn-after-reload case issue #91 fixed for the gateway). A no-op for a
 * pool without http.tls_cert. Returns 0 or -1; on -1 the caller must not
 * start accepting.
 *
 * `on_accept` is called with `on_accept_arg` for each accepted connection,
 * before the bufferevent is built. It exists because evhttp offers exactly one
 * per-connection hook and on a TLS pool this module owns it: the caller's
 * accept gate (issue #53) has to travel with the bevcb so that a certificate
 * reload, which reinstalls the pair, does not silently drop it. May be NULL. */
int fpm_http_direct_tls_child_attach(struct fpm_worker_pool_s *wp, struct event_base *base,
	struct evhttp *http, void (*on_accept)(void *), void *on_accept_arg);

/* Whether this pool terminates TLS, i.e. whether REQUEST_SCHEME is https and
 * HTTPS is on for every request it serves. Answers from configuration, so it
 * is the same answer in the master and in a child. */
bool fpm_http_direct_tls_enabled(struct fpm_worker_pool_s *wp);

#endif
