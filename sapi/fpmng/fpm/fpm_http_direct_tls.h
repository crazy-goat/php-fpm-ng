/* fpm-ng: TLS termination for pool.type = http-direct, both executors
 * (issue #55).
 *
 * Deliberately thin. The gateway already terminates TLS and already reloads a
 * certificate without restarting anything, and all of that lives behind two
 * headers that know nothing about gateways: fpm_tls_http.h (validate, load in
 * the master, build an SSL_CTX per process) and fpm_tls_reload.h
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

#include <event2/util.h>

struct fpm_worker_pool_s;
struct event_base;
struct evhttp;
struct bufferevent;

/* This contract has two implementations, picked at build time by
 * --enable-fpmng-tls (issue #280): fpm_tls_http_direct.c, which terminates
 * TLS, and the stubs in fpm_http_direct_tls.c, which refuse a TLS pool at
 * startup and are no-ops for everything else. Callers see one set of
 * functions and carry no #ifdef.
 */

/* Config validation, in the master, before anything forks. Refuses a
 * half-configured pair, refuses TLS at all in a build made without
 * --enable-fpmng-tls (naming the flag rather than serving plain HTTP on a
 * port the operator configured as HTTPS), and otherwise hands
 * cert/key/min_version/sni to fpm_tls_http_validate(), which is the same
 * check the `http` pool type gets. Returns 0 or -1, logging by itself. */
int fpm_http_direct_tls_validate(struct fpm_worker_pool_s *wp);

/* The part of the check above that is the same in both builds, because it
 * only reads the configuration: cert without key, key without cert, and the
 * knobs that only mean anything once a certificate is served. Lives in
 * fpm_http_direct_tls.c and is shared with fpm_tls_http_direct.c; not for
 * anyone else. Returns 0 or -1, logging by itself. */
int fpm_http_direct_tls_validate_pairing(struct fpm_worker_pool_s *wp);

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
 * `on_accept` is called with `on_accept_arg` and the connection's freshly
 * built bufferevent (NULL if it could not be built) for each accepted
 * connection. It exists because evhttp offers exactly one per-connection hook
 * and on a TLS pool this module owns it: the caller's accept gate (issue #53)
 * and its connection tracking (issue #61) have to travel with the bevcb so
 * that a certificate reload, which reinstalls the pair, does not silently drop
 * them. May be NULL. */
int fpm_http_direct_tls_child_attach(struct fpm_worker_pool_s *wp, struct event_base *base,
	struct evhttp *http, void (*on_accept)(void *, struct bufferevent *), void *on_accept_arg);

/* Issue #195. The streaming writer's write step for a TLS pool: puts the front
 * of the connection's output buffer on the wire with SSL_write() instead of
 * writing it to the descriptor, which on a TLS connection would put plaintext
 * there. Returns the bytes accepted (> 0), 0 if the write blocked (*poll_events
 * then says what to wait for), FPM_HTTP_DIRECT_TLS_WRITE_IDLE when the buffer's
 * front held nothing to write, or -1 on an error that ends the connection.
 * Never blocks and never runs the event loop -- see fpm_tls_http_write_output()
 * for why the caller may not.
 *
 * In a build made without --enable-fpmng-tls this returns -1, which no pool
 * can reach: a pool with http.tls_cert is refused at startup there, and a pool
 * without one never gets this write step.
 *
 * Kept here rather than in fpm_http_direct.c so that the executor has one write
 * step per transport and no #ifdef of its own. */
/* Declared outside the TLS build guard, because the caller compares against it
 * in both builds. Its value is FPM_TLS_HTTP_WRITE_IDLE's, which is checked in
 * fpm_tls_http_direct.c where both headers are in scope. */
#define FPM_HTTP_DIRECT_TLS_WRITE_IDLE (-2)

ev_ssize_t fpm_http_direct_tls_write(struct bufferevent *bev, short *poll_events);

/* Issue #458. The executor owns the bounded event-loop wait; the TLS layer owns
 * the OpenSSL step. These values mirror fpm_tls_http.h's shutdown results so
 * the executor carries no OpenSSL types or #ifdef. */
#define FPM_HTTP_DIRECT_TLS_SHUTDOWN_NOT_APPLICABLE 0
#define FPM_HTTP_DIRECT_TLS_SHUTDOWN_PENDING 1
#define FPM_HTTP_DIRECT_TLS_SHUTDOWN_SENT 2
#define FPM_HTTP_DIRECT_TLS_SHUTDOWN_FAILED 3

int fpm_http_direct_tls_shutdown_step(struct bufferevent *bev, short *poll_events);

/* Issue #195. Tells libevent that a response written by fpm_http_direct_tls_write()
 * has left the buffer, which is what makes evhttp finish the request. Call it
 * only with the response complete AND the output buffer empty; per write, or
 * with bytes still queued, it finishes the request on top of data that has not
 * been sent yet. A no-op in a build without --enable-fpmng-tls and on a plaintext
 * pool, whose write step goes through the descriptor libevent is watching. */
void fpm_http_direct_tls_notify_written(struct bufferevent *bev);

/* Whether this pool terminates TLS, i.e. whether REQUEST_SCHEME is https and
 * HTTPS is on for every request it serves. Answers from configuration, so it
 * is the same answer in the master and in a child. */
bool fpm_http_direct_tls_enabled(struct fpm_worker_pool_s *wp);

#endif
