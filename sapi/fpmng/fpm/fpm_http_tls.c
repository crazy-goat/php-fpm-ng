/* fpm-ng: TLS termination for the HTTP gateway, see fpm_http_tls.h.
 *
 * Compiled in only when config.m4 found both libevent_openssl and OpenSSL
 * (HAVE_FPM_HTTP_TLS); otherwise this whole file is an empty translation
 * unit and http.tls_cert is refused at config-validation time in fpm_http.c.
 *
 * Cert and key are read into memory once, in the master, before any gateway
 * child is forked (fpm_http_tls_load()); every child inherits those bytes
 * through fork() and builds its OWN SSL_CTX from them (fpm_http_tls_ctx_new()),
 * so the private key file itself is only ever opened by the master, and the
 * gateway process -- which forks straight off the master and never drops
 * privileges (see fpm_http_gateway_run()) -- never has to reopen it.
 */

#include "fpm_config.h"

#ifdef HAVE_FPM_HTTP_TLS

#include "fpm_http_tls.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <event2/bufferevent_ssl.h>

#include "zlog.h"

/* Slurps a whole file into a malloc'd, NUL-terminated buffer. Never logs the
 * content, only ever the path, on failure. */
static char *fpm_http_tls_read_file(const char *path, size_t *out_len)
{
	FILE *f;
	long size;
	char *buf;
	size_t got;

	f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)size + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	got = size > 0 ? fread(buf, 1, (size_t)size, f) : 0;
	fclose(f);
	if (got != (size_t)size) {
		free(buf);
		return NULL;
	}
	buf[size] = '\0';
	if (out_len) {
		*out_len = (size_t)size;
	}
	return buf;
}

/* "" or NULL -> TLSv1.2 (domyslna). Nieznana nazwa -> -1, caller loguje. */
static int fpm_http_tls_resolve_min_version(const char *min_version)
{
	if (!min_version || !*min_version || strcmp(min_version, "TLSv1.2") == 0) {
		return TLS1_2_VERSION;
	}
	if (strcmp(min_version, "TLSv1.3") == 0) {
		return TLS1_3_VERSION;
	}
	return -1;
}

/* Parsuje cert+klucz z pamieci do jednorazowego SSL_CTX i sprawdza, ze
 * pasuja do siebie. Zwraca 0/-1, `what` opisuje co sie nie udalo (do
 * komunikatu wolajacego), NIGDY tresc klucza. */
static int fpm_http_tls_check(const char *cert_pem, size_t cert_len, const char *key_pem, size_t key_len,
	const char **what)
{
	SSL_CTX *ctx;
	BIO *bio;
	X509 *cert = NULL;
	EVP_PKEY *key = NULL;

	*what = NULL;
	ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx) {
		*what = "cannot allocate SSL_CTX";
		return -1;
	}

	bio = BIO_new_mem_buf(cert_pem, (int)cert_len);
	if (bio) {
		cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
		BIO_free(bio);
	}
	if (!cert) {
		*what = "certificate is not a valid PEM X.509 certificate";
		goto out;
	}
	if (SSL_CTX_use_certificate(ctx, cert) != 1) {
		*what = "certificate rejected by OpenSSL";
		goto out;
	}

	bio = BIO_new_mem_buf(key_pem, (int)key_len);
	if (bio) {
		key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
		BIO_free(bio);
	}
	if (!key) {
		*what = "private key is not a valid PEM key";
		goto out;
	}
	if (SSL_CTX_use_PrivateKey(ctx, key) != 1) {
		*what = "private key rejected by OpenSSL";
		goto out;
	}
	if (SSL_CTX_check_private_key(ctx) != 1) {
		*what = "private key does not match the certificate";
		goto out;
	}
out:
	if (cert) {
		X509_free(cert);
	}
	if (key) {
		EVP_PKEY_free(key);
	}
	SSL_CTX_free(ctx);
	return *what ? -1 : 0;
}

int fpm_http_tls_validate(const char *pool, const char *cert_path, const char *key_path,
	const char *min_version)
{
	char *cert_pem, *key_pem;
	size_t cert_len, key_len;
	const char *what = NULL;
	int ret;

	if (fpm_http_tls_resolve_min_version(min_version) < 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.tls_min_version '%s' is not one of TLSv1.2, TLSv1.3", pool, min_version);
		return -1;
	}

	cert_pem = fpm_http_tls_read_file(cert_path, &cert_len);
	if (!cert_pem) {
		zlog(ZLOG_ERROR, "[pool %s] http.tls_cert: cannot read '%s'", pool, cert_path);
		return -1;
	}
	key_pem = fpm_http_tls_read_file(key_path, &key_len);
	if (!key_pem) {
		free(cert_pem);
		zlog(ZLOG_ERROR, "[pool %s] http.tls_key: cannot read '%s'", pool, key_path);
		return -1;
	}

	ret = fpm_http_tls_check(cert_pem, cert_len, key_pem, key_len, &what);
	free(cert_pem);
	free(key_pem);
	if (ret != 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.tls_cert/http.tls_key: %s", pool, what);
		return -1;
	}
	return 0;
}

struct fpm_http_tls_s *fpm_http_tls_load(const char *pool, const char *cert_path,
	const char *key_path, const char *min_version)
{
	struct fpm_http_tls_s *tls;
	const char *what = NULL;

	tls = calloc(1, sizeof(*tls));
	if (!tls) {
		return NULL;
	}

	tls->min_version = fpm_http_tls_resolve_min_version(min_version);
	if (tls->min_version < 0) {
		/* fpm_http_tls_validate() already refused this at config-validation
		 * time; reaching this means the config changed underneath us. */
		zlog(ZLOG_ERROR, "[pool %s] http.tls_min_version '%s' is not one of TLSv1.2, TLSv1.3", pool, min_version);
		free(tls);
		return NULL;
	}

	tls->cert_pem = fpm_http_tls_read_file(cert_path, &tls->cert_len);
	tls->key_pem = tls->cert_pem ? fpm_http_tls_read_file(key_path, &tls->key_len) : NULL;
	if (!tls->cert_pem || !tls->key_pem) {
		zlog(ZLOG_ERROR, "[pool %s] http: cannot re-read TLS certificate/key at startup", pool);
		fpm_http_tls_free(tls);
		return NULL;
	}

	if (fpm_http_tls_check(tls->cert_pem, tls->cert_len, tls->key_pem, tls->key_len, &what) != 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.tls_cert/http.tls_key: %s", pool, what);
		fpm_http_tls_free(tls);
		return NULL;
	}

	/* Wspolny klucz session ticketow dla WSZYSTKICH gateway procesow tego
	 * poola: generowany raz, tutaj, w masterze, PRZED forkiem pierwszego
	 * dziecka -- fork() kopiuje `tls` (a wiec i ten klucz) do kazdego
	 * dziecka, ktore ustawia go w swoim WLASNYM SSL_CTX
	 * (fpm_http_tls_ctx_new()). Bez tego kazdy proces bramki mialby wlasny,
	 * losowy klucz i klient trafiajacy raz w jeden proces, raz w drugi
	 * (SO_REUSEPORT) placilby pelny handshake za kazdym razem. */
	if (RAND_bytes(tls->ticket_key, sizeof(tls->ticket_key)) != 1) {
		zlog(ZLOG_ERROR, "[pool %s] http: RAND_bytes() failed generating the TLS session ticket key", pool);
		fpm_http_tls_free(tls);
		return NULL;
	}

	return tls;
}

void fpm_http_tls_free(struct fpm_http_tls_s *tls)
{
	if (!tls) {
		return;
	}
	free(tls->cert_pem);
	free(tls->key_pem);
	free(tls);
}

SSL_CTX *fpm_http_tls_ctx_new(const char *pool, struct fpm_http_tls_s *tls)
{
	SSL_CTX *ctx;
	BIO *bio;
	X509 *cert = NULL;
	EVP_PKEY *key = NULL;

	ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx) {
		zlog(ZLOG_ERROR, "[pool %s] http: SSL_CTX_new() failed", pool);
		return NULL;
	}

	bio = BIO_new_mem_buf(tls->cert_pem, (int)tls->cert_len);
	if (bio) {
		cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
		BIO_free(bio);
	}
	bio = BIO_new_mem_buf(tls->key_pem, (int)tls->key_len);
	if (bio) {
		key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
		BIO_free(bio);
	}
	if (!cert || !key || SSL_CTX_use_certificate(ctx, cert) != 1 || SSL_CTX_use_PrivateKey(ctx, key) != 1 ||
			SSL_CTX_check_private_key(ctx) != 1) {
		/* Already validated once in the master (fpm_http_tls_load()); getting
		 * here means something changed the in-memory bytes, which should be
		 * impossible -- fail loudly rather than silently serve plain HTTP. */
		zlog(ZLOG_ERROR, "[pool %s] http: cannot rebuild TLS context in gateway child", pool);
		if (cert) {
			X509_free(cert);
		}
		if (key) {
			EVP_PKEY_free(key);
		}
		SSL_CTX_free(ctx);
		return NULL;
	}
	X509_free(cert);
	EVP_PKEY_free(key);

	SSL_CTX_set_min_proto_version(ctx, tls->min_version);
	SSL_CTX_set_session_id_context((const unsigned char*)"fpm-ng", 6);
	SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
	SSL_CTX_set_tlsext_ticket_keys(ctx, tls->ticket_key, sizeof(tls->ticket_key));

	return ctx;
}

struct bufferevent *fpm_http_tls_bevcb(struct event_base *base, void *arg)
{
	SSL_CTX *ctx = arg;
	SSL *ssl = SSL_new(ctx);

	return bufferevent_openssl_socket_new(base, -1, ssl, BUFFEREVENT_SSL_ACCEPTING, BEV_OPT_CLOSE_ON_FREE);
}

#endif /* HAVE_FPM_HTTP_TLS */
