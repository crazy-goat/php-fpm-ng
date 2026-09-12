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

#include <poll.h>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <event2/buffer.h>
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

/* "" or NULL -> TLSv1.2 (default). Unknown name -> -1; the caller logs it. */
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

/* Installs every X.509 block found in cert_pem into ctx, in file order: the
 * first block as the leaf (SSL_CTX_use_certificate(), same as before this
 * fix), every block after it as a chain certificate
 * (SSL_CTX_add_extra_chain_cert()) so a fullchain.pem's intermediates are
 * actually sent to the client instead of being read into memory
 * (fpm_http_tls_load()) and then never installed -- that was the bug
 * (task 039). Shared by fpm_http_tls_check() (throwaway validation ctx) and
 * fpm_http_tls_ctx_new() (the real per-child ctx), so both parse and both
 * serve exactly the same way.
 *
 * Decision (039, acceptance criterion 4): this does NOT verify that block 2
 * is the issuer of block 1 or that the chain terminates anywhere -- it only
 * requires each block to parse as an X.509 certificate. A file whose blocks
 * are not actually a valid chain is therefore installed and served exactly
 * as given, byte for byte, in file order; a broken chain shows up at the
 * client (openssl s_client, curl, ...), not here. Rejecting it here would
 * need path-building (X509_verify_cert() against a trust store), which does
 * not exist at load time and would be a second, different kind of check from
 * "does this key match this leaf" below.
 *
 * SSL_CTX_use_certificate() does not take ownership of the X509 it is given
 * (caller must free it); SSL_CTX_add_extra_chain_cert() DOES take ownership
 * of every cert passed to it (SSL_CTX_free() frees them), so those must not
 * be freed here. */
static int fpm_http_tls_install_chain(SSL_CTX *ctx, const char *cert_pem, size_t cert_len, const char **what)
{
	BIO *bio;
	X509 *leaf;
	X509 *extra;

	bio = BIO_new_mem_buf(cert_pem, (int)cert_len);
	if (!bio) {
		*what = "cannot allocate BIO for the certificate PEM";
		return -1;
	}

	leaf = PEM_read_bio_X509(bio, NULL, NULL, NULL);
	if (!leaf) {
		BIO_free(bio);
		*what = "certificate is not a valid PEM X.509 certificate";
		return -1;
	}
	if (SSL_CTX_use_certificate(ctx, leaf) != 1) {
		X509_free(leaf);
		BIO_free(bio);
		*what = "certificate rejected by OpenSSL";
		return -1;
	}
	X509_free(leaf);

	while ((extra = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
		if (SSL_CTX_add_extra_chain_cert(ctx, extra) != 1) {
			X509_free(extra);
			BIO_free(bio);
			*what = "intermediate certificate rejected by OpenSSL";
			return -1;
		}
	}
	/* PEM_read_bio_X509() returning NULL here is the ordinary "no more PEM
	 * blocks in the BIO" end condition, not necessarily an error; it leaves
	 * PEM_R_NO_START_LINE on OpenSSL's error queue, which would otherwise
	 * surface as a bogus error the next time something calls ERR_get_error()
	 * (e.g. libevent's TLS error logging on a later, unrelated handshake). */
	ERR_clear_error();
	BIO_free(bio);

	return 0;
}

/* Parse cert+key from memory into a throwaway SSL_CTX and verify that they
 * match. Returns 0/-1; `what` describes what failed (for the caller's error
 * message), NEVER key material. */
static int fpm_http_tls_check(const char *cert_pem, size_t cert_len, const char *key_pem, size_t key_len,
	const char **what)
{
	SSL_CTX *ctx;
	BIO *bio;
	EVP_PKEY *key = NULL;

	*what = NULL;
	ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx) {
		*what = "cannot allocate SSL_CTX";
		return -1;
	}

	if (fpm_http_tls_install_chain(ctx, cert_pem, cert_len, what) != 0) {
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
	if (key) {
		EVP_PKEY_free(key);
	}
	SSL_CTX_free(ctx);
	return *what ? -1 : 0;
}

/* One parsed http.tls_sni_cert entry, "servername:cert_path:key_path", before
 * any file is opened -- just the split-out strings. */
struct fpm_http_tls_sni_spec_s {
	char *servername;
	char *cert_path;
	char *key_path;
};

static void fpm_http_tls_sni_spec_free(struct fpm_http_tls_sni_spec_s *specs, size_t count)
{
	size_t i;

	if (!specs) {
		return;
	}
	for (i = 0; i < count; i++) {
		free(specs[i].servername);
		free(specs[i].cert_path);
		free(specs[i].key_path);
	}
	free(specs);
}

/* Trims leading/trailing spaces and tabs in place, returns the trimmed start. */
static char *fpm_http_tls_trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t') {
		s++;
	}
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
		*--end = '\0';
	}
	return s;
}

/* Splits http.tls_sni_cert ("servername:cert:key,servername:cert:key,...")
 * into heap-allocated {servername, cert_path, key_path} triples. Empty/NULL
 * spec -> *out_count = 0, *out = NULL, success. A malformed entry (not
 * exactly 2 colons) fails with *what set to a static, ready-to-log message
 * naming the bad entry text (never key material -- this is a path spec, not
 * a PEM); caller frees whatever was built so far via
 * fpm_http_tls_sni_spec_free(). Shared by fpm_http_tls_validate() and
 * fpm_http_tls_load() so the syntax is parsed in exactly one place. */
static int fpm_http_tls_sni_parse(const char *spec, struct fpm_http_tls_sni_spec_s **out,
	size_t *out_count, const char **what)
{
	/* `what` on the malformed-entry path must outlive `dup`, which is freed
	 * before this function returns; a static buffer is simpler than asking
	 * every caller to free a heap copy for what is, in practice, a
	 * config-validation-time error logged once and never retried
	 * concurrently (this is master-process startup/reload code only). */
	static char bad_entry[256];
	char *dup, *saveptr, *entry;
	struct fpm_http_tls_sni_spec_s *specs = NULL;
	size_t count = 0, cap = 0;

	*out = NULL;
	*out_count = 0;
	*what = NULL;

	if (!spec || !*spec) {
		return 0;
	}

	dup = strdup(spec);
	if (!dup) {
		*what = "out of memory";
		return -1;
	}

	for (entry = strtok_r(dup, ",", &saveptr); entry; entry = strtok_r(NULL, ",", &saveptr)) {
		char *trimmed = fpm_http_tls_trim(entry);
		char *first_colon, *second_colon;
		struct fpm_http_tls_sni_spec_s *slot;

		if (!*trimmed) {
			continue; /* tolerate a trailing comma / empty entry */
		}

		first_colon = strchr(trimmed, ':');
		second_colon = first_colon ? strchr(first_colon + 1, ':') : NULL;
		if (!first_colon || !second_colon || strchr(second_colon + 1, ':')) {
			snprintf(bad_entry, sizeof(bad_entry), "malformed entry '%s' (want servername:cert_path:key_path)", trimmed);
			*what = bad_entry;
			fpm_http_tls_sni_spec_free(specs, count);
			free(dup);
			return -1;
		}

		if (count == cap) {
			size_t new_cap = cap ? cap * 2 : 4;
			struct fpm_http_tls_sni_spec_s *grown = realloc(specs, new_cap * sizeof(*specs));

			if (!grown) {
				*what = "out of memory";
				fpm_http_tls_sni_spec_free(specs, count);
				free(dup);
				return -1;
			}
			specs = grown;
			cap = new_cap;
		}

		slot = &specs[count];
		*first_colon = '\0';
		*second_colon = '\0';
		/* trimmed was truncated in place at first_colon, above, but only the
		 * OUTER whitespace of the whole entry was stripped so far
		 * (fpm_http_tls_trim() at the top of this loop) -- trim again so
		 * "servername : cert : key" (spaces around the colons) does not
		 * leave a trailing space baked into the servername SNI actually
		 * matches against. */
		slot->servername = strdup(fpm_http_tls_trim(trimmed));
		slot->cert_path = strdup(fpm_http_tls_trim(first_colon + 1));
		slot->key_path = strdup(fpm_http_tls_trim(second_colon + 1));
		if (!slot->servername || !slot->cert_path || !slot->key_path) {
			free(slot->servername);
			free(slot->cert_path);
			free(slot->key_path);
			*what = "out of memory";
			fpm_http_tls_sni_spec_free(specs, count);
			free(dup);
			return -1;
		}
		count++;
	}

	free(dup);
	*out = specs;
	*out_count = count;
	return 0;
}

int fpm_http_tls_validate(const char *pool, const char *cert_path, const char *key_path,
	const char *min_version, const char *sni_spec)
{
	char *cert_pem, *key_pem;
	size_t cert_len, key_len;
	const char *what = NULL;
	int ret;
	struct fpm_http_tls_sni_spec_s *specs = NULL;
	size_t spec_count = 0, i;

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

	if (fpm_http_tls_sni_parse(sni_spec, &specs, &spec_count, &what) != 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.tls_sni_cert: %s", pool, what);
		return -1;
	}

	for (i = 0; i < spec_count; i++) {
		char *sni_cert_pem, *sni_key_pem;
		size_t sni_cert_len, sni_key_len;

		sni_cert_pem = fpm_http_tls_read_file(specs[i].cert_path, &sni_cert_len);
		if (!sni_cert_pem) {
			zlog(ZLOG_ERROR, "[pool %s] http.tls_sni_cert: %s: cannot read '%s'", pool, specs[i].servername, specs[i].cert_path);
			fpm_http_tls_sni_spec_free(specs, spec_count);
			return -1;
		}
		sni_key_pem = fpm_http_tls_read_file(specs[i].key_path, &sni_key_len);
		if (!sni_key_pem) {
			free(sni_cert_pem);
			zlog(ZLOG_ERROR, "[pool %s] http.tls_sni_cert: %s: cannot read '%s'", pool, specs[i].servername, specs[i].key_path);
			fpm_http_tls_sni_spec_free(specs, spec_count);
			return -1;
		}

		ret = fpm_http_tls_check(sni_cert_pem, sni_cert_len, sni_key_pem, sni_key_len, &what);
		free(sni_cert_pem);
		free(sni_key_pem);
		if (ret != 0) {
			zlog(ZLOG_ERROR, "[pool %s] http.tls_sni_cert: %s: %s", pool, specs[i].servername, what);
			fpm_http_tls_sni_spec_free(specs, spec_count);
			return -1;
		}
	}

	fpm_http_tls_sni_spec_free(specs, spec_count);
	return 0;
}

struct fpm_http_tls_s *fpm_http_tls_load(const char *pool, const char *cert_path,
	const char *key_path, const char *min_version, const char *sni_spec)
{
	struct fpm_http_tls_s *tls;
	const char *what = NULL;
	struct fpm_http_tls_sni_spec_s *specs = NULL;
	size_t spec_count = 0, i;

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

	/* Shared session ticket key for ALL gateway processes of this pool:
	 * generated once, here, in the master, BEFORE the first child forks —
	 * fork() copies `tls` (and thus this key) into every child, which sets
	 * it in its OWN SSL_CTX (fpm_http_tls_ctx_new()). Without this every
	 * gateway process would have its own random key, and a client hitting
	 * one process and then the other (SO_REUSEPORT) would pay the full
	 * handshake every time. */
	if (RAND_bytes(tls->ticket_key, sizeof(tls->ticket_key)) != 1) {
		zlog(ZLOG_ERROR, "[pool %s] http: RAND_bytes() failed generating the TLS session ticket key", pool);
		fpm_http_tls_free(tls);
		return NULL;
	}

	/* http.tls_sni_cert (task 041): same defensive re-read-and-re-check
	 * pattern as the primary cert/key pair just above, in case the files on
	 * disk changed between fpm_http_tls_validate() and here. */
	if (fpm_http_tls_sni_parse(sni_spec, &specs, &spec_count, &what) != 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.tls_sni_cert: %s", pool, what);
		fpm_http_tls_free(tls);
		return NULL;
	}

	if (spec_count > 0) {
		tls->sni = calloc(spec_count, sizeof(*tls->sni));
		if (!tls->sni) {
			zlog(ZLOG_ERROR, "[pool %s] http.tls_sni_cert: out of memory", pool);
			fpm_http_tls_sni_spec_free(specs, spec_count);
			fpm_http_tls_free(tls);
			return NULL;
		}
	}

	for (i = 0; i < spec_count; i++) {
		struct fpm_http_tls_sni_s *slot = &tls->sni[tls->sni_count];

		slot->servername = strdup(specs[i].servername);
		slot->cert_pem = fpm_http_tls_read_file(specs[i].cert_path, &slot->cert_len);
		slot->key_pem = slot->cert_pem ? fpm_http_tls_read_file(specs[i].key_path, &slot->key_len) : NULL;
		if (!slot->servername || !slot->cert_pem || !slot->key_pem) {
			zlog(ZLOG_ERROR, "[pool %s] http.tls_sni_cert: %s: cannot re-read certificate/key at startup", pool, specs[i].servername);
			tls->sni_count++; /* so fpm_http_tls_free() below frees this half-filled slot too */
			fpm_http_tls_sni_spec_free(specs, spec_count);
			fpm_http_tls_free(tls);
			return NULL;
		}
		if (fpm_http_tls_check(slot->cert_pem, slot->cert_len, slot->key_pem, slot->key_len, &what) != 0) {
			zlog(ZLOG_ERROR, "[pool %s] http.tls_sni_cert: %s: %s", pool, specs[i].servername, what);
			tls->sni_count++;
			fpm_http_tls_sni_spec_free(specs, spec_count);
			fpm_http_tls_free(tls);
			return NULL;
		}
		tls->sni_count++;
	}

	fpm_http_tls_sni_spec_free(specs, spec_count);
	return tls;
}

void fpm_http_tls_free(struct fpm_http_tls_s *tls)
{
	size_t i;

	if (!tls) {
		return;
	}
	free(tls->cert_pem);
	free(tls->key_pem);
	for (i = 0; i < tls->sni_count; i++) {
		free(tls->sni[i].servername);
		free(tls->sni[i].cert_pem);
		free(tls->sni[i].key_pem);
	}
	free(tls->sni);
	free(tls);
}

/* ALPN wire format (RFC 7301): a list of length-prefixed protocol name
 * strings; 8 = strlen("http/1.1"). Exactly one entry -- this is the server's
 * preference list, and the gateway speaks HTTP/1.1 and nothing else
 * (docs/NOTES.md, "What we do NOT do": no HTTP/2). */
static const unsigned char fpm_http_tls_alpn_protos[] = "\x08http/1.1";

/* SSL_CTX_set_alpn_select_cb() callback, registered on every SSL_CTX this
 * file ever builds (default and per-SNI, see fpm_http_tls_build_ctx()) so
 * ALPN negotiates the same way regardless of which certificate a connection
 * ends up on. SSL_select_next_proto() picks the first protocol in the
 * SERVER's list (fpm_http_tls_alpn_protos) that also appears in the CLIENT's
 * list (in/inlen) -- server preference, not client preference, is what
 * decides here since our list has exactly one entry anyway.
 *
 * OPENSSL_NPN_NO_OVERLAP means the client sent an ALPN extension but did not
 * offer http/1.1: reject the handshake outright (task 041 acceptance
 * criterion 2) instead of silently falling back to serving HTTP/1.1 anyway,
 * which would ignore what the client explicitly asked for. A client that
 * sends NO ALPN extension at all never reaches this callback -- unaffected,
 * served as HTTP/1.1 by assumption, exactly today's (pre-041) behaviour. */
static int fpm_http_tls_alpn_select_cb(SSL *ssl, const unsigned char **out, unsigned char *outlen,
	const unsigned char *in, unsigned int inlen, void *arg)
{
	(void) ssl;
	(void) arg;

	if (SSL_select_next_proto((unsigned char **) out, outlen, fpm_http_tls_alpn_protos,
			sizeof(fpm_http_tls_alpn_protos) - 1, in, inlen) == OPENSSL_NPN_NO_OVERLAP) {
		return SSL_TLSEXT_ERR_ALERT_FATAL;
	}
	return SSL_TLSEXT_ERR_OK;
}

/* Builds one cert+key+options SSL_CTX from PEM bytes already in memory:
 * install the chain, set and check the private key, minimum protocol
 * version, session id context, SSL_OP_NO_COMPRESSION, the shared ticket key,
 * and the ALPN select callback above. Shared by the default ctx and every
 * per-SNI-name ctx in fpm_http_tls_ctx_new() (task 041) -- both need exactly
 * this construction, and building it in one place means an SNI certificate
 * can never end up silently missing one of these settings (e.g. the ALPN
 * callback, or a different min_version) that the default certificate has.
 * `log_name` identifies which certificate failed, for the caller's error
 * message: the pool name for the default ctx, "pool %s, http.tls_sni_cert
 * %s" for a per-SNI one. */
static SSL_CTX *fpm_http_tls_build_ctx(const char *log_name, const char *cert_pem, size_t cert_len,
	const char *key_pem, size_t key_len, int min_version, const unsigned char ticket_key[80])
{
	SSL_CTX *ctx;
	BIO *bio;
	EVP_PKEY *key = NULL;
	const char *what = NULL;

	ctx = SSL_CTX_new(TLS_server_method());
	if (!ctx) {
		zlog(ZLOG_ERROR, "[%s] http: SSL_CTX_new() failed", log_name);
		return NULL;
	}

	/* Already validated once in the master (fpm_http_tls_load(), which calls
	 * the same fpm_http_tls_check() / fpm_http_tls_install_chain()); getting
	 * a failure here means something changed the in-memory bytes since then,
	 * which should be impossible -- fail loudly rather than silently serve
	 * plain HTTP or an incomplete chain. */
	if (fpm_http_tls_install_chain(ctx, cert_pem, cert_len, &what) != 0) {
		zlog(ZLOG_ERROR, "[%s] http: cannot rebuild TLS context in gateway child: %s", log_name, what);
		SSL_CTX_free(ctx);
		return NULL;
	}

	bio = BIO_new_mem_buf(key_pem, (int) key_len);
	if (bio) {
		key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
		BIO_free(bio);
	}
	if (!key || SSL_CTX_use_PrivateKey(ctx, key) != 1 || SSL_CTX_check_private_key(ctx) != 1) {
		zlog(ZLOG_ERROR, "[%s] http: cannot rebuild TLS context in gateway child", log_name);
		if (key) {
			EVP_PKEY_free(key);
		}
		SSL_CTX_free(ctx);
		return NULL;
	}
	EVP_PKEY_free(key);

	SSL_CTX_set_min_proto_version(ctx, min_version);
	SSL_CTX_set_session_id_context(ctx, (const unsigned char*)"fpm-ng", 6);
	SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
	if (SSL_CTX_set_tlsext_ticket_keys(ctx, (void *) ticket_key, 80) != 1) {
		zlog(ZLOG_ERROR, "[%s] http: cannot set the shared TLS session ticket key", log_name);
		SSL_CTX_free(ctx);
		return NULL;
	}
	SSL_CTX_set_alpn_select_cb(ctx, fpm_http_tls_alpn_select_cb, NULL);

	return ctx;
}

/* One servername -> SSL_CTX* entry in the per-process SNI switch table
 * built by fpm_http_tls_ctx_new() below. */
struct fpm_http_tls_sni_ctx_entry_s {
	char *servername;
	SSL_CTX *ctx;
};

struct fpm_http_tls_sni_ctx_table_s {
	struct fpm_http_tls_sni_ctx_entry_s *entries;
	size_t count;
};

/* SSL_CTX_set_tlsext_servername_callback() callback, registered only on the
 * DEFAULT ctx (there would be no point registering it anywhere else: SNI
 * extension processing happens before any per-SNI ctx is selected). `arg` is
 * the table SSL_CTX_set_tlsext_servername_arg() was given, built once by
 * fpm_http_tls_ctx_new(). */
static int fpm_http_tls_sni_select_cb(SSL *ssl, int *al, void *arg)
{
	struct fpm_http_tls_sni_ctx_table_s *table = arg;
	const char *requested;
	size_t i;

	(void) al;

	requested = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
	if (requested) {
		/* Hostnames are case-insensitive (RFC 4343); http.tls_sni_cert
		 * entries are matched the same way a client typing the name in
		 * either case would expect. */
		for (i = 0; i < table->count; i++) {
			if (strcasecmp(requested, table->entries[i].servername) == 0) {
				SSL_set_SSL_CTX(ssl, table->entries[i].ctx);
				break;
			}
		}
	}
	/* No SNI at all, or a name not in the table: `ssl` is already on the
	 * default ctx (SSL_new() put it there) -- this IS the documented
	 * fallback to the default certificate (task 041 acceptance criteria 3
	 * and 4), not a missing case. */
	return SSL_TLSEXT_ERR_OK;
}

SSL_CTX *fpm_http_tls_ctx_new(const char *pool, struct fpm_http_tls_s *tls)
{
	SSL_CTX *ctx;
	struct fpm_http_tls_sni_ctx_table_s *table;
	size_t i;

	ctx = fpm_http_tls_build_ctx(pool, tls->cert_pem, tls->cert_len, tls->key_pem, tls->key_len,
		tls->min_version, tls->ticket_key);
	if (!ctx) {
		return NULL;
	}

	if (tls->sni_count == 0) {
		return ctx;
	}

	table = malloc(sizeof(*table));
	if (!table) {
		zlog(ZLOG_ERROR, "[pool %s] http: out of memory building the SNI certificate table", pool);
		SSL_CTX_free(ctx);
		return NULL;
	}
	table->entries = calloc(tls->sni_count, sizeof(*table->entries));
	table->count = 0;
	if (!table->entries) {
		zlog(ZLOG_ERROR, "[pool %s] http: out of memory building the SNI certificate table", pool);
		free(table);
		SSL_CTX_free(ctx);
		return NULL;
	}

	for (i = 0; i < tls->sni_count; i++) {
		char log_name[256];
		SSL_CTX *sni_ctx;

		snprintf(log_name, sizeof(log_name), "pool %s, http.tls_sni_cert %s", pool, tls->sni[i].servername);
		sni_ctx = fpm_http_tls_build_ctx(log_name, tls->sni[i].cert_pem, tls->sni[i].cert_len,
			tls->sni[i].key_pem, tls->sni[i].key_len, tls->min_version, tls->ticket_key);
		if (!sni_ctx) {
			/* fpm_http_tls_build_ctx() already logged which one failed.
			 * Reaching here should be impossible -- fpm_http_tls_load()
			 * already validated these same bytes -- but an SNI certificate
			 * failing to (re)build must fail the whole gateway process the
			 * same way the default certificate failing does, not silently
			 * serve fewer certificates than configured. */
			size_t j;

			for (j = 0; j < table->count; j++) {
				free(table->entries[j].servername);
				SSL_CTX_free(table->entries[j].ctx);
			}
			free(table->entries);
			free(table);
			SSL_CTX_free(ctx);
			return NULL;
		}
		table->entries[i].servername = strdup(tls->sni[i].servername);
		table->entries[i].ctx = sni_ctx;
		table->count++;
	}

	/* Deliberate simplification (task 041): this table, and every per-name
	 * SSL_CTX* inside it, is intentionally never freed. It lives for the
	 * lifetime of the gateway child process; fpm_http_tls_ctx_new() rebuilds
	 * a brand new table (and a brand new set of SSL_CTX*s) every time it
	 * runs again -- once at child startup, and again if task 040's
	 * hot-reload rebuilds the default ctx (see fpm_http_tls_reload.c) -- and
	 * the previous table simply leaks. That leak is small and bounded (one
	 * entry per configured SNI name) and happens at most once every
	 * http.tls_reload_check seconds; inventing an ex_data-based
	 * free-on-SSL_CTX_free() lifetime hook to reclaim it is not worth it for
	 * a handful of certificates. This leak is the accepted design, not an
	 * oversight. */
	SSL_CTX_set_tlsext_servername_callback(ctx, fpm_http_tls_sni_select_cb);
	SSL_CTX_set_tlsext_servername_arg(ctx, table);

	return ctx;
}

/* One TLS record's worth per SSL_write(): 16384 is SSL3_RT_MAX_PLAIN_LENGTH,
 * the most plaintext OpenSSL puts in one record, so a call maps to at most one
 * record on the wire. Deliberately NOT libevent's WRITE_FRAME, which is 15000
 * (bufferevent_openssl.c:731) and does not bound its SSL_write() anyway --
 * do_write() overwrites that argument (bufferevent_openssl.c:664) and passes
 * the whole peeked iov_len, because evbuffer_peek() does not trim the last
 * vector to the requested length (buffer.c, evbuffer_peek: iov_len =
 * chain->off). Bigger here would not put more on the wire and would make a
 * blocked write hold a larger promise that every retry has to keep. */
#define FPM_HTTP_TLS_WRITE_FRAME 16384

/* The other half of libevent's do_write(), and the reason it cannot live in the
 * write step above: libevent ends a successful write with
 * bufferevent_trigger_nolock_(bev, EV_WRITE, BEV_OPT_DEFER_CALLBACKS)
 * (bufferevent_openssl.c:727), and that callback is how evhttp finds out a
 * response has left -- evhttp_write_cb() runs evcon->cb, which for a finished
 * reply is evhttp_send_done(). Nothing else fires it on this path:
 * consider_writing() calls do_write() only while the buffer still holds
 * something (bufferevent_openssl.c:877), so a buffer emptied by us leaves the
 * request open forever. Measured on the test box: after fpmng_respond() on a
 * TLS pool the client got its response and then never got an answer to the
 * next request on the same keep-alive connection (4 s client timeout), while
 * the same script on a plaintext pool answered it in 0.30 s.
 *
 * Called only where the response is complete, never per write. Per write it
 * truncates: the callback is deferred, the loop runs it after the request
 * callback returns, and by then the terminating chunk has been queued but not
 * yet written -- evhttp_write_cb() does not look at the buffer, so
 * evhttp_send_done() finishes the request on top of it. Measured the same way:
 * a 16 MiB streamed body arrived exactly 5 bytes short, without its "0\r\n\r\n".
 *
 * Deferred rather than immediate because evhttp_send_done() frees the request
 * and the caller is inside that request's own callback. */
void fpm_http_tls_notify_written(struct bufferevent *bev)
{
	bufferevent_trigger(bev, EV_WRITE, BEV_TRIG_DEFER_CALLBACKS);
}

ev_ssize_t fpm_http_tls_write_output(struct bufferevent *bev, short *poll_events)
{
	struct evbuffer *out = bufferevent_get_output(bev);
	SSL *ssl = bufferevent_openssl_get_ssl(bev);
	struct evbuffer_iovec vec[FPM_HTTP_TLS_WRITE_VECS];
	ev_ssize_t written;
	size_t len;
	int r, n, i;

	*poll_events = POLLOUT;
	if (!ssl) {
		return -1;
	}
	/* No unfreeze here, and above all no freeze: unlike the plaintext step,
	 * which has to lift the bufferevent's own freeze to drain the buffer, an
	 * OpenSSL bufferevent never freezes anything. The freeze of the output
	 * front belongs to bufferevent_socket_new() (bufferevent_sock.c:373) and
	 * bufferevent_openssl.c contains no evbuffer_freeze() call at all, which
	 * is why its do_write() (bufferevent_openssl.c:654) drains the buffer
	 * directly. Freezing it here left it frozen for libevent: the drain at the
	 * end of do_write() returns -1 without removing anything (buffer.c,
	 * evbuffer_drain: `if (buf->freeze_start) return -1`) while SSL_write()
	 * keeps succeeding, so consider_writing() loops on a buffer that never
	 * empties. Measured on the test box: after a correct 16 MiB body the pool
	 * wrote the 5-byte terminating chunk about 542,000 times -- 2.7 MB of
	 * "0\r\n\r\n" -- with strace showing back-to-back 27-byte TLS records and
	 * no poll() between them. */
	/* Several vectors, and empty ones skipped, because a chain with off == 0
	 * in front of the data is a case libevent guards against explicitly in the
	 * same loop: "SSL_write will (reasonably) return 0 if we tell it to send 0
	 * data. Skip this case so we don't interpret the result as an error"
	 * (bufferevent_openssl.c:663). Peeking one vector and handing that zero
	 * length to the caller as "blocked" would spin: the pump would poll() a
	 * socket that is already writable, spend no measurable time, and never
	 * reach its http.stream_write_timeout. */
	n = evbuffer_peek(out, FPM_HTTP_TLS_WRITE_FRAME, NULL, vec, FPM_HTTP_TLS_WRITE_VECS);
	if (n > FPM_HTTP_TLS_WRITE_VECS) {
		n = FPM_HTTP_TLS_WRITE_VECS;
	}
	for (i = 0; i < n && vec[i].iov_len == 0; i++) {
		/* nothing: an empty chain in front of the data */
	}
	if (i >= n) {
		return FPM_HTTP_TLS_WRITE_IDLE;
	}
	len = vec[i].iov_len < FPM_HTTP_TLS_WRITE_FRAME ? vec[i].iov_len : FPM_HTTP_TLS_WRITE_FRAME;
	/* Cleared before, not read after a success: SSL_get_error() is only
	 * meaningful against a fresh queue, and a stale entry from an earlier
	 * handshake would otherwise be reported as this write's failure. */
	ERR_clear_error();
	r = SSL_write(ssl, vec[i].iov_base, (int) len);
	if (r > 0) {
		/* Drains from the front, which is why the empty chains skipped above
		 * are not left behind: they hold no bytes, so the drain frees them and
		 * takes r bytes out of the first chain that has any. */
		evbuffer_drain(out, (size_t) r);
		return (ev_ssize_t) r;
	}
	written = 0;
	switch (SSL_get_error(ssl, r)) {
	case SSL_ERROR_WANT_WRITE:
		break;
	case SSL_ERROR_WANT_READ:
		/* A renegotiation: the write cannot finish until the peer's half of it
		 * arrives, so waiting for writability would wait forever. */
		*poll_events = POLLIN;
		break;
	default:
		written = -1;
		break;
	}
	return written;
}

struct bufferevent *fpm_http_tls_bevcb(struct event_base *base, void *arg)
{
	SSL_CTX *ctx = arg;
	SSL *ssl = SSL_new(ctx);

	return bufferevent_openssl_socket_new(base, -1, ssl, BUFFEREVENT_SSL_ACCEPTING, BEV_OPT_CLOSE_ON_FREE);
}

#endif /* HAVE_FPM_HTTP_TLS */
