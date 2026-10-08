/* fpm-ng: issue #661 -- gateway listeners across the reload's execvp().
 *
 * A reload is a full execvp() of the master (fpm_process_ctl.c,
 * fpm_pctl_exec()). Before this issue fpm_http_cleanup() closed the gateway
 * listeners just before that exec, and the next master bound them again. A
 * client that connected in that gap got ECONNREFUSED.
 *
 * Now the old master keeps its TCP listening sockets open across the exec:
 * fpm_http_handoff_export() clears FD_CLOEXEC on them and writes one record
 * per socket into FPMNG_HTTP_LISTENERS, "<fd>:<tls>:<bind>", comma-separated.
 * The socket never leaves LISTEN state, so a connection that arrives during
 * the exec waits in the kernel's accept queue. The new generation accepts it.
 *
 * The next master takes a socket over only when the bind text and the TLS
 * fingerprint (fpm_http_handoff_tls_fp()) both match the gateway that asks for
 * the listener, and only when that gateway listens at once (not
 * http.tls_wait_for_cert's NO_CERT state). Any other inherited socket is
 * closed, and the listener is bound as before. The bind text is compared as
 * written: "0.0.0.0:8080" and "*:8080" do not match, so they rebind.
 *
 * Not covered, on purpose:
 *  - http.reuseport = on: each gateway child binds its own SO_REUSEPORT socket,
 *    and the master socket is closed during the exec as before.
 *  - a unix public listener: it is not recorded.
 *  - an execvp() that fails after fpm_reload_config_check() passed. The old
 *    generation is already drained, and the master exits. A failure at the
 *    check is different: the reload is refused (fpm_reload_config_check.h).
 */

#ifndef FPM_HTTP_HANDOFF_H
#define FPM_HTTP_HANDOFF_H 1

struct fpm_worker_pool_s;

/* Environment variable that carries the listeners across the exec. The next
 * master reads it and unsets it in fpm_http_handoff_begin(), so no child
 * inherits it. */
#define FPM_HTTP_HANDOFF_ENV "FPMNG_HTTP_LISTENERS"

/* Writes the fingerprint of the http.tls_* settings of wp into out: 16 lower
 * case hex digits and a NUL. It is FNV-1a 64-bit over the certificate, key,
 * minimum version, SNI, client verification and client CA values, and over
 * http.tls_wait_for_cert. Two configurations with the same fingerprint are
 * the same TLS setup for the purpose of adopting a listener. */
void fpm_http_handoff_tls_fp(const struct fpm_worker_pool_s *wp, char out[17]);

/* The next master, called from fpm_init() after the configuration is read and
 * before fpm_sockets_init_main(). Every pool type binds its socket after this
 * call, so an inherited gateway socket that no gateway of the configuration
 * uses is closed before it can block a bind of another pool type (a fastcgi
 * pool on the old gateway address, for example). Reads FPM_HTTP_HANDOFF_ENV and
 * unsets it. Returns 0; the chain in fpm_init() tests the result like the
 * other init steps. */
int fpm_http_handoff_begin(void);

/* The next master, called once after the pools are initialized and before the
 * first child is forked. Closes the inherited sockets that no gateway took
 * over, and frees the table. */
void fpm_http_handoff_finish(void);

/* The old master, from fpm_http_cleanup() at FPM_CLEANUP_PARENT_EXEC, after the
 * gateways drained. Hands every TCP listener of a proxy_only gateway that does
 * not use reuseport to the next generation. A handed socket is set to -1 in its
 * gateway, so the caller does not close it. */
void fpm_http_handoff_export(void);

/* Replaces fpm_http_listen() for the public and the plain listener of a
 * gateway. Takes over the inherited socket for http_address when its TLS
 * fingerprint is tls_fp and do_listen is set, and closes any other inherited
 * socket for the same address before it binds. Falls back to
 * fpm_http_listen(), with the same arguments and result. */
int fpm_http_handoff_listen(const char *pool, const char *listen_address, const char *http_address, int backlog, int reuseport, int do_listen, const char *tls_fp);

#endif
