/* fpm-ng: TLS termination for the HTTP gateway (http.tls_cert/http.tls_key), see fpm_http_tls.c.
 *
 * Compiled in only when the build found both libevent's OpenSSL glue and
 * OpenSSL itself (config.m4); HAVE_FPM_HTTP_TLS is fpm_config.h's signal for
 * that. Without it every declaration below is compiled out and fpm_http.c's
 * own HAVE_FPM_HTTP_TLS guards keep it from calling any of this, so plain
 * HTTP keeps working and http.tls_cert is refused at config-validation time
 * with a message naming what is missing.
 */

#ifndef FPM_HTTP_TLS_H
#define FPM_HTTP_TLS_H 1

#ifdef HAVE_FPM_HTTP_TLS

#include <stddef.h>
#include <openssl/ssl.h>
#include <event2/event.h>
#include <event2/bufferevent.h>

/* One SNI-selected certificate (task 041), on top of the default
 * cert_pem/key_pem below. Same fork()-copied-once-in-the-master property as
 * the default pair: read from disk in fpm_http_tls_load(), never reopened by
 * a gateway child. */
struct fpm_http_tls_sni_s {
	char *servername;
	char *cert_pem;
	size_t cert_len;
	char *key_pem;
	size_t key_len;
};

/* Validated TLS material for one gateway family (one pool), fully resolved
 * BEFORE fork() of any gateway child:
 *   - cert_pem/key_pem are the PEM files read into memory once, in the
 *     master (fpm_http_tls_load()); fork() copies them into every child, so
 *     no child ever reopens the key file from disk.
 *   - ticket_key is 48 random bytes generated once, in the master, with
 *     RAND_bytes(); fork() copies it too, so every gateway child's SSL_CTX
 *     (built separately per process, see fpm_http_tls_ctx_new()) uses the
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
 *     fpm_http_tls_ctx_new(). This struct only ever holds bytes, the same
 *     bytes the default cert_pem/key_pem already hold; it is not the
 *     machinery the decision is about.
 * There is deliberately no SSL_CTX here: SSL_CTX is built by
 * fpm_http_tls_ctx_new() in each child, never in the master, never inherited
 * through fork().
 */
struct fpm_http_tls_s {
	char *cert_pem;
	size_t cert_len;
	char *key_pem;
	size_t key_len;
	int min_version;			/* np. TLS1_2_VERSION, patrz fpm_http_tls.c */
	/* 80 = 16 (key name) + 32 (AES-256 key) + 32 (HMAC-SHA256 key), the
	 * layout OpenSSL's classic SSL_CTX_set_tlsext_ticket_keys() expects
	 * since it moved off AES-128/HMAC-SHA1 -- the old 48-byte layout from
	 * early OpenSSL 1.x docs is refused with "invalid ticket keys length". */
	unsigned char ticket_key[80];
	struct fpm_http_tls_sni_s *sni;		/* http.tls_sni_cert, parsed; NULL when unset */
	size_t sni_count;
};

/* Wolane z fpm_http_validate_pool(), w fazie walidacji configu, przed
 * forkiem czegokolwiek: czyta cert+klucz z dysku do jednorazowego, rzucanego
 * SSL_CTX i sprawdza, ze sie parsuja i ze klucz pasuje do certyfikatu.
 * Nic nie zostaje w pamieci. Komunikat bledu mowi co jest nie tak (zla
 * sciezka, zly PEM, klucz nie pasuje do certyfikatu, nieznana
 * min_version) i NIGDY nie zawiera tresci klucza. Zwraca 0 albo -1,
 * loguje sam.
 *
 * sni_spec is http.tls_sni_cert (task 041), possibly NULL/empty: each
 * "servername:cert_path:key_path" entry is validated exactly like the
 * primary cert_path/key_path pair above, using the same checks. */
int fpm_http_tls_validate(const char *pool, const char *cert_path, const char *key_path,
	const char *min_version, const char *sni_spec);

/* Wolane raz na pool, w masterze, PRZED forkiem pierwszego dziecka bramki
 * (fpm_http_init_pool_ex()): czyta cert+klucz do pamieci (fork() skopiuje je
 * do kazdego dziecka) i generuje wspolny klucz session ticketow. Zaklada, ze
 * fpm_http_tls_validate() juz przeszla dla tych samych sciezek/sni_spec.
 * Zwraca NULL przy bledzie (zalogowanym), nigdy nie zwraca czesciowo
 * wypelnionej struktury. sni_spec: patrz fpm_http_tls_validate() powyzej;
 * wypelnia tls->sni/tls->sni_count. */
struct fpm_http_tls_s *fpm_http_tls_load(const char *pool, const char *cert_path,
	const char *key_path, const char *min_version, const char *sni_spec);

void fpm_http_tls_free(struct fpm_http_tls_s *tls);

/* Wolane w kazdym dziecku bramki, PO forku (fpm_http_gateway_run()): buduje
 * SSL_CTX z bajtow juz wczytanych w masterze (zero ponownego czytania pliku
 * klucza z dysku) i ustawia w nim wspolny klucz ticketow. Zwraca NULL przy
 * bledzie (zalogowanym pod nazwa poola).
 *
 * task 041: also registers an ALPN callback (advertises http/1.1 only,
 * rejects a client offering only something else) and, when tls->sni_count >
 * 0, an SNI servername callback backed by a per-process switch table of
 * additional SSL_CTX*s built here -- both are per-process state, built fresh
 * every time this runs, never carried across fork() or stored in
 * struct fpm_http_tls_s. See fpm_http_tls.c for the switch-table lifetime
 * note. */
SSL_CTX *fpm_http_tls_ctx_new(const char *pool, struct fpm_http_tls_s *tls);

/* Callback dla evhttp_set_bevcb(): `arg` to SSL_CTX* danego gateway procesu.
 * evhttp wola to raz na kazde przychodzace polaczenie i samo doczepia
 * przyjety fd przez bufferevent_setfd() -- stad fd = -1 tutaj. */
struct bufferevent *fpm_http_tls_bevcb(struct event_base *base, void *arg);

#endif /* HAVE_FPM_HTTP_TLS */

#endif
