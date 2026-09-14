/* fpm-ng: TLS termination for the HTTP gateway (http.tls_cert/http.tls_key), see fpm_tls_http.c.
 *
 * Compiled in only when the build found both libevent's OpenSSL glue and
 * OpenSSL itself (config.m4); HAVE_FPM_HTTP_TLS is fpm_config.h's signal for
 * that. Without it every declaration below is compiled out and fpm_http.c's
 * own HAVE_FPM_HTTP_TLS guards keep it from calling any of this, so plain
 * HTTP keeps working and http.tls_cert is refused at config-validation time
 * with a message naming what is missing.
 */

#ifndef FPM_TLS_HTTP_H
#define FPM_TLS_HTTP_H 1

#ifdef HAVE_FPM_HTTP_TLS

#include <stddef.h>
#include <openssl/ssl.h>
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/util.h>

/* One SNI-selected certificate (task 041), on top of the default
 * cert_pem/key_pem below. Same fork()-copied-once-in-the-master property as
 * the default pair: read from disk in fpm_tls_http_load(), never reopened by
 * a gateway child. */
struct fpm_tls_http_sni_s {
	char *servername;
	char *cert_pem;
	size_t cert_len;
	char *key_pem;
	size_t key_len;
};

/* Validated TLS material for one gateway family (one pool), fully resolved
 * BEFORE fork() of any gateway child:
 *   - cert_pem/key_pem are the PEM files read into memory once, in the
 *     master (fpm_tls_http_load()); fork() copies them into every child, so
 *     no child ever reopens the key file from disk.
 *   - ticket_key is 48 random bytes generated once, in the master, with
 *     RAND_bytes(); fork() copies it too, so every gateway child's SSL_CTX
 *     (built separately per process, see fpm_tls_http_ctx_new()) uses the
 *     SAME session ticket key and a client can resume a session against
 *     whichever gateway process SO_REUSEPORT happens to hand it next time.
 *   - sni/sni_count (task 041) are the same kind of thing as cert_pem/key_pem
 *     above: raw PEM bytes for http.tls_sni_cert's extra certificates, read
 *     once in the master and inherited unchanged by every child via fork().
 *     This is fine to keep here even though the task's decision text says
 *     the certificate-*selection* state must be per-process, not a new field
 *     of this struct -- what must stay per-process is the OpenSSL selection
 *     machinery (the SNI servername callback and its switch table of
 *     per-name SSL_CTX*), which IS built separately, per process, in
 *     fpm_tls_http_ctx_new(). This struct only ever holds bytes, the same
 *     bytes the default cert_pem/key_pem already hold; it is not the
 *     machinery the decision is about.
 * There is deliberately no SSL_CTX here: SSL_CTX is built by
 * fpm_tls_http_ctx_new() in each child, never in the master, never inherited
 * through fork().
 */
struct fpm_tls_http_s {
	char *cert_pem;
	size_t cert_len;
	char *key_pem;
	size_t key_len;
	int min_version;			/* e.g. TLS1_2_VERSION, see fpm_tls_http.c */
	/* 80 = 16 (key name) + 32 (AES-256 key) + 32 (HMAC-SHA256 key), the
	 * layout OpenSSL's classic SSL_CTX_set_tlsext_ticket_keys() expects
	 * since it moved off AES-128/HMAC-SHA1 -- the old 48-byte layout from
	 * early OpenSSL 1.x docs is refused with "invalid ticket keys length". */
	unsigned char ticket_key[80];
	struct fpm_tls_http_sni_s *sni;		/* http.tls_sni_cert, parsed; NULL when unset */
	size_t sni_count;
	/* mTLS (issue #62). verify_client: 0 = off (default, no CertificateRequest
	 * sent, matches every pool's behavior before this), 1 = "optional" (a
	 * certificate is requested but its absence does not fail the handshake),
	 * 2 = "require" (the handshake fails without one). client_ca_pem/_len are
	 * the http.tls_client_ca bytes, read once in the master exactly like
	 * cert_pem/key_pem above; NULL/0 when verify_client is 0. */
	int verify_client;
	char *client_ca_pem;
	size_t client_ca_len;
};

/* Called from fpm_http_validate_pool(), during config validation, before
 * anything forks: reads cert+key from disk into a throwaway SSL_CTX and
 * verifies that they parse and that the key matches the certificate. Nothing
 * stays in memory. The error message says what is wrong (bad path, bad PEM,
 * key does not match certificate, unknown min_version) and NEVER contains key
 * material. Returns 0 or -1, logging by itself.
 *
 * sni_spec is http.tls_sni_cert (task 041), possibly NULL/empty: each
 * "servername:cert_path:key_path" entry is validated exactly like the
 * primary cert_path/key_path pair above, using the same checks. */
int fpm_tls_http_validate(const char *pool, const char *cert_path, const char *key_path,
	const char *min_version, const char *sni_spec,
	const char *verify_client, const char *client_ca_path);

/* Called once per pool, in the master, BEFORE the first gateway child forks
 * (fpm_http_init_pool_ex()): reads cert+key into memory (fork() copies them
 * into every child) and generates the shared session ticket key. Assumes
 * fpm_tls_http_validate() already passed for the same paths/sni_spec.
 * Returns NULL on error (logged), never a partially filled structure.
 * sni_spec: see fpm_tls_http_validate() above; fills tls->sni/tls->sni_count. */
struct fpm_tls_http_s *fpm_tls_http_load(const char *pool, const char *cert_path,
	const char *key_path, const char *min_version, const char *sni_spec,
	const char *verify_client, const char *client_ca_path);

void fpm_tls_http_free(struct fpm_tls_http_s *tls);

/* Called in every gateway child, AFTER the fork (fpm_http_gateway_run()):
 * builds an SSL_CTX from bytes already loaded in the master (no key file is
 * read from disk again) and sets the shared ticket key in it. Returns NULL on
 * error (logged under the pool name).
 *
 * task 041: also registers an ALPN callback (advertises http/1.1 only,
 * rejects a client offering only something else) and, when tls->sni_count >
 * 0, an SNI servername callback backed by a per-process switch table of
 * additional SSL_CTX*s built here -- both are per-process state, built fresh
 * every time this runs, never carried across fork() or stored in
 * struct fpm_tls_http_s. See fpm_tls_http.c for the switch-table lifetime
 * note. */
SSL_CTX *fpm_tls_http_ctx_new(const char *pool, struct fpm_tls_http_s *tls);

/* Callback for evhttp_set_bevcb(): `arg` is the SSL_CTX* of the gateway
 * process. evhttp calls it once per incoming connection and attaches the
 * accepted fd itself via bufferevent_setfd() — hence fd = -1 here. */
struct bufferevent *fpm_tls_http_bevcb(struct event_base *base, void *arg);

/* Issue #195. Puts the front of an SSL bufferevent's own output buffer on the
 * wire with SSL_write(), for a caller that is not allowed to run the event
 * loop and so cannot let libevent do it: inside an evhttp request callback a
 * nested event_base_loop() on the base being dispatched is refused, and
 * libevent 2.1's bufferevent_flush() is a no-op on an SSL bufferevent
 * (be_openssl_flush() is an "XXXX Implement this" stub,
 * bufferevent_openssl.c:1259). Reaching past the bufferevent to the descriptor
 * -- what the plaintext path does -- is not an option here, because that
 * buffer holds plaintext and the descriptor carries the session.
 *
 * One record's worth per call, the same shape libevent's own do_write() has
 * (bufferevent_openssl.c:654): peek the front of the output buffer, SSL_write
 * it, drain what was accepted. Safe to interleave with libevent's writer for
 * two reasons, and only for those two: in socket mode -- which is what
 * fpm_tls_http_bevcb() builds -- libevent writes only from its write event, so
 * it cannot be mid-write while the caller holds the loop; and libevent sets
 * SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER (bufferevent_openssl.c:1369), so a write
 * OpenSSL left pending may be retried from a different address, provided the
 * retry is at least as long -- which is why this function hands OpenSSL the
 * peeked vector's whole length, exactly as do_write() does, instead of
 * clamping it (see FPM_TLS_HTTP_WRITE_FRAME in fpm_tls_http.c).
 *
 * Returns the plaintext bytes accepted (> 0), 0 if the write blocked -- then
 * *poll_events is POLLOUT, or POLLIN when a renegotiation made the write wait
 * on a read -- FPM_TLS_HTTP_WRITE_IDLE when there was nothing to hand OpenSSL,
 * or -1 on an error that ends the connection. Never blocks. */
/* Neither progress nor a reason to wait: the buffer's front held no bytes to
 * write. A caller that treated this as "blocked" would wait on a descriptor
 * that is already writable; one that treated it as an error would drop a live
 * connection. Same value and same meaning as FPM_DIRECT_WRITE_IDLE, which is
 * what fpm_http_direct_tls_write() maps it to. */
#define FPM_TLS_HTTP_WRITE_IDLE (-2)
/* How many vectors the write step peeks at once. Eight, as libevent's
 * do_write() does (bufferevent_openssl.c:669), so that empty chains in front
 * of the data can be skipped rather than reported as a zero-length write. */
#define FPM_TLS_HTTP_WRITE_VECS 8

ev_ssize_t fpm_tls_http_write_output(struct bufferevent *bev, short *poll_events);

/* Tells libevent -- and through it evhttp -- that a response this module wrote
 * itself has left the buffer, which is what finishes the request. Call it once
 * the response is complete and the buffer is empty, NOT after each write; see
 * the definition for what each mistake costs. */
void fpm_tls_http_notify_written(struct bufferevent *bev);

#endif /* HAVE_FPM_HTTP_TLS */

#endif
