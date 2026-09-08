/* fpm-ng: reload the HTTP gateway's TLS certificate/key without restarting
 * any gateway process, see fpm_http_tls_reload.h. Design (task 040, full
 * reasoning in task 040, done; see docs/task-archive.md):
 *
 * - The master owns the file reads, same property fpm_http_tls.h already
 *   documents for startup -- children never open the key file, here or
 *   anywhere else. A self-rearming timer in the master (fpm_events.c's own
 *   epoll-based loop, the same one fpm_pctl_heartbeat() uses) digests
 *   cert_path/key_path every http.tls_reload_check seconds. Never in the
 *   request path. That digest replaced a whole-second st_mtime comparison,
 *   which could not see a pair replaced inside one second (issue #71) --
 *   fpm_http_tls_reload_file_digest() carries the reasoning.
 * - On a content change the master calls the existing, already-stateless
 *   fpm_http_tls_validate() against the candidate paths BEFORE publishing
 *   anything a child can see (torn-read backstop: a half-written pair fails
 *   validation and is simply skipped, logged, retried next tick). Only a
 *   validated pair is read again with fpm_http_tls_load() and copied into
 *   shared memory.
 * - Propagation is a double-buffered region (fpm_shm_alloc(), plain
 *   MAP_SHARED with no locking) holding two fixed-size slots plus one
 *   atomic generation counter: the master always writes into the slot NOT
 *   currently published, then bumps the counter -- the bump is the only
 *   thing a reader needs to observe, and it always points at a fully
 *   written slot. Each gateway child has its OWN timer, on its OWN
 *   event_base, that cheaply compares the counter against what it last
 *   adopted; on a change it rebuilds its SSL_CTX with the existing,
 *   unmodified fpm_http_tls_ctx_new() and re-registers it with
 *   evhttp_set_bevcb() -- gw->listen_fd and gw->base are never touched, and
 *   OpenSSL reference-counts SSL_CTX internally, so freeing the old one does
 *   not disturb connections already in flight against it.
 * - Session resumption falls out for free: ticket_key travels inside the
 *   same published slot, so every child adopts the same one at the same
 *   generation bump.
 */

#include "fpm_config.h"

#ifdef HAVE_FPM_HTTP_TLS

#include "fpm_http_tls_reload.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <openssl/evp.h>
#include <openssl/crypto.h>

#include "fpm_events.h"
#include "fpm_shm.h"
#include "fpm_atomic.h"
#include "zlog.h"

/* One published TLS material set. cert_pem/key_pem are fixed-size, not
 * malloc'd: this lives in a fpm_shm_alloc() region shared by mmap() across
 * the master and every gateway child of the pool, so nothing here can be a
 * pointer into process-private heap memory. */
struct fpm_http_tls_reload_slot_s {
	size_t cert_len;
	size_t key_len;
	int min_version;
	unsigned char ticket_key[80];		/* see fpm_http_tls.h for the 80 = 16+32+32 layout */
	char cert_pem[FPM_HTTP_TLS_RELOAD_MAX_CERT];
	char key_pem[FPM_HTTP_TLS_RELOAD_MAX_KEY];
};

struct fpm_http_tls_reload_shared_s {
	/* index = generation % 2 is the slot currently in effect. The master is
	 * the only writer (single self-rearming timer, never re-entrant), so
	 * this only needs to be visible across processes, not arbitrated
	 * between concurrent writers -- atomic_cmp_set() is used anyway, to
	 * match the one other existing multi-process pattern in this codebase
	 * (gw->upstreams_used, fpm_http.c) rather than inventing a second idiom. */
	atomic_t generation;
	struct fpm_http_tls_reload_slot_s slot[2];
};

struct fpm_http_tls_reload_s {
	char *pool;
	char *cert_path;
	char *key_path;
	char *min_version;
	int check_interval_sec;

	/* master-only: SHA-256 of the cert/key bytes this timer last acted on
	 * (published, or rejected -- see the tick), and the timer itself. The
	 * identity is the CONTENT, not st_mtime: see
	 * fpm_http_tls_reload_file_digest() below for why (issue #71). All-zero
	 * until the first successful read, which is not a digest any real file
	 * can produce, so a pair that could not be read at startup is picked up
	 * by the first tick that can read it. */
	unsigned char cert_digest[FPM_HTTP_TLS_RELOAD_DIGEST_LEN];
	unsigned char key_digest[FPM_HTTP_TLS_RELOAD_DIGEST_LEN];
	struct fpm_event_s master_timer;

	/* shared across the master and every gateway child of this pool */
	struct fpm_http_tls_reload_shared_s *shared;

	/* task 041: http.tls_sni_cert's parsed PEM bytes, BORROWED (not owned,
	 * not deep-copied, not part of the shared-memory slot) from `initial`
	 * (the same struct fpm_http_tls_s fpm_http_tls_load() built once at
	 * gateway startup and that fpm_http_tls_reload_master_init() below
	 * receives as its `initial` argument -- gw->tls in fpm_http.c). This
	 * borrow is safe because gw->tls is never freed while the gateway child
	 * that owns this fpm_http_tls_reload_s stays alive: fpm_http_tls_free()
	 * is called on gw->tls nowhere in fpm_http.c (checked every call site of
	 * fpm_http_tls_free() there before relying on this) -- gw->tls lives in
	 * the master for the master's entire lifetime, and every gateway child
	 * is a fork() of the master, so the bytes stay valid for exactly as long
	 * as this pointer is used (fpm_http_tls_reload_child_tick() below).
	 * SNI certificates themselves are NOT hot-reloaded (only the primary
	 * cert/key are, unchanged from task 040) -- a changed http.tls_sni_cert
	 * path or file needs a restart, same as before this task existed. */
	struct fpm_http_tls_sni_s *sni;
	size_t sni_count;

	/* child-only: private per gateway process after fork(), never read or
	 * written by the master or by any sibling gateway process. */
	unsigned long last_seen_generation;
	struct event *child_timer;
	struct evhttp *child_http;
	SSL_CTX **child_ctx_slot;
	/* task 031: after a reload the listener must keep its bevcb WRAPPER
	 * (fpm_http.c arms the per-connection read deadline there), with the
	 * gateway as arg -- the wrapper reads the new *child_ctx_slot itself.
	 * Reinstalling fpm_http_tls_bevcb directly would silently drop the
	 * read deadline from every connection accepted after the reload. */
	struct bufferevent *(*child_bevcb)(struct event_base *, void *);
	void *child_bevcb_arg;
	char local_cert_pem[FPM_HTTP_TLS_RELOAD_MAX_CERT];
	char local_key_pem[FPM_HTTP_TLS_RELOAD_MAX_KEY];
};

/* SHA-256 of one file's bytes, issue #71. What this replaces is a comparison
 * of st_mtime, which is a time_t -- WHOLE SECONDS -- so a certificate/key pair
 * replaced inside the same wall-clock second as the pair before it was
 * indistinguishable from "nothing changed" and was silently never adopted:
 * measured as 2 of 5 failing runs of build/test-http-tls-reload.sh on the test
 * box on 2026-09-08, and as CI build-matrix runs 34139815139, 34140641442,
 * 34121726613, 34120910345 and 34120774897.
 *
 * st_mtim.tv_nsec was rejected: several filesystems and NFS mounts leave it
 * zero or coarse, so it would turn the bug from "always" into "sometimes"
 * rather than fix it. A content digest does not depend on the clock at all,
 * and it answers the question this tick actually has -- "are these the bytes
 * I already acted on?" -- so a rollback to a previous pair is correctly a
 * change, while a rewrite of identical bytes is correctly nothing.
 *
 * Three things this does that stat() got for free and a read does not:
 *
 * - Only regular files. O_NONBLOCK on the open() so that a path that turned
 *   into a FIFO cannot block the master's event loop inside open() waiting
 *   for a writer -- the same loop fpm_pctl_heartbeat() runs on.
 * - At most `max_bytes` are read. A cert path pointing at something huge (a
 *   mistyped path, a log file) must not turn a tick into a bulk read every
 *   http.tls_reload_check seconds; such a pair is over
 *   FPM_HTTP_TLS_RELOAD_MAX_CERT/MAX_KEY and is unpublishable anyway.
 * - st_size is mixed into the digest, so a change past that read cap is
 *   still a change: an oversized file therefore still reaches the existing
 *   "larger than the reload buffer" path in the tick, which logs it once.
 *
 * Cost for a real certificate: two small files read per http.tls_reload_check
 * seconds (default 5) in the master only. Never in the request path, and
 * never in a child -- children do not open the key file, the property
 * fpm_http_tls.h documents.
 *
 * Returns 0 and fills `out` on success, -1 on any read or digest failure. The
 * digest is never logged; `out` is the only thing that leaves this function,
 * and the buffer that held key bytes is wiped before returning. */
static int fpm_http_tls_reload_file_digest(const char *path, size_t max_bytes,
	unsigned char out[FPM_HTTP_TLS_RELOAD_DIGEST_LEN]) /* {{{ */
{
	int fd;
	struct stat st;
	FILE *fp;
	EVP_MD_CTX *ctx;
	unsigned char buf[4096];
	size_t left;
	size_t n;
	unsigned int len = 0;
	int ok = 0;
	uint64_t size_le;

	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		return -1;
	}
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return -1;
	}
	fp = fdopen(fd, "rb");
	if (!fp) {
		close(fd);
		return -1;
	}
	ctx = EVP_MD_CTX_new();
	if (!ctx) {
		fclose(fp);
		return -1;
	}

	if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1) {
		ok = 1;
		/* Byte order fixed on purpose: the digest is only ever compared
		 * against another digest taken by this same process, but a
		 * platform-independent encoding costs nothing and keeps the value
		 * meaningful if it is ever published. */
		size_le = (uint64_t) st.st_size;
		for (n = 0; n < sizeof(size_le); n++) {
			buf[n] = (unsigned char) ((size_le >> (8 * n)) & 0xff);
		}
		if (EVP_DigestUpdate(ctx, buf, sizeof(size_le)) != 1) {
			ok = 0;
		}

		left = max_bytes;
		while (ok && left > 0 && (n = fread(buf, 1, left < sizeof(buf) ? left : sizeof(buf), fp)) > 0) {
			if (EVP_DigestUpdate(ctx, buf, n) != 1) {
				ok = 0;
				break;
			}
			left -= n;
		}
		if (ok && ferror(fp)) {
			ok = 0;
		}
		if (ok && EVP_DigestFinal_ex(ctx, out, &len) != 1) {
			ok = 0;
		}
	}

	OPENSSL_cleanse(buf, sizeof(buf));
	EVP_MD_CTX_free(ctx);
	fclose(fp);

	return (ok && len == FPM_HTTP_TLS_RELOAD_DIGEST_LEN) ? 0 : -1;
}
/* }}} */

static void fpm_http_tls_reload_master_tick(struct fpm_event_s *ev, short which, void *arg) /* {{{ */
{
	struct fpm_http_tls_reload_s *r = arg;
	unsigned char cert_digest[FPM_HTTP_TLS_RELOAD_DIGEST_LEN];
	unsigned char key_digest[FPM_HTTP_TLS_RELOAD_DIGEST_LEN];
	struct fpm_http_tls_s *fresh;
	unsigned long gen;
	unsigned target;

	(void) ev;
	if (which != FPM_EV_TIMEOUT) {
		return;
	}

	if (fpm_http_tls_reload_file_digest(r->cert_path, FPM_HTTP_TLS_RELOAD_MAX_CERT, cert_digest) != 0 ||
			fpm_http_tls_reload_file_digest(r->key_path, FPM_HTTP_TLS_RELOAD_MAX_KEY, key_digest) != 0) {
		/* Transient (mid write, or the volume is not there yet): not fatal,
		 * skip this tick, the certificate already in use keeps serving. */
		return;
	}
	if (memcmp(cert_digest, r->cert_digest, sizeof(cert_digest)) == 0 &&
			memcmp(key_digest, r->key_digest, sizeof(key_digest)) == 0) {
		return;
	}

	/* The digests above were taken from a read that precedes the validate and
	 * the load below, so a write landing between them is remembered as the
	 * OLDER content while the NEWER content is what gets published. That
	 * direction is the safe one: the next tick sees a mismatch and reloads
	 * the same bytes once more (idempotent), rather than remembering content
	 * it never published and skipping it forever. */

	/* Torn reads, task 040 decision: enforced, not just documented. Reuses
	 * the exact check config-validation already runs at startup -- never
	 * logs key material. */
	/* sni_spec: NULL -- this tick only ever re-checks http.tls_cert/
	 * http.tls_key's mtimes (task 040's original scope), never
	 * http.tls_sni_cert's paths (task 041's scope cut, see
	 * fpm_http_tls_reload_s.sni/sni_count below): SNI certificates are not
	 * hot-reloaded, only the primary cert/key are. */
	if (fpm_http_tls_validate(r->pool, r->cert_path, r->key_path, r->min_version, NULL) != 0) {
		/* fpm_http_tls_validate() already logged what's wrong. Remember
		 * these digests anyway so a persistently broken pair (operator
		 * hasn't fixed it yet) does not re-log every tick; the next actual
		 * change of the bytes re-triggers validation -- including a corrected
		 * pair written in the same second as the broken one, which the old
		 * mtime comparison could not see (issue #71). */
		memcpy(r->cert_digest, cert_digest, sizeof(r->cert_digest));
		memcpy(r->key_digest, key_digest, sizeof(r->key_digest));
		return;
	}

	fresh = fpm_http_tls_load(r->pool, r->cert_path, r->key_path, r->min_version, NULL);
	if (!fresh) {
		/* fpm_http_tls_load() already logged (re-read failed between the
		 * validate above and here, or RAND_bytes() failed) -- do not update
		 * the digests, so this is retried next tick rather than skipped. */
		return;
	}

	if (fresh->cert_len > FPM_HTTP_TLS_RELOAD_MAX_CERT || fresh->key_len > FPM_HTTP_TLS_RELOAD_MAX_KEY) {
		zlog(ZLOG_ERROR, "[pool %s] http.tls_reload_check: new certificate/key is larger than the %u/%u byte reload buffer, keeping the certificate already in use",
			r->pool, (unsigned) FPM_HTTP_TLS_RELOAD_MAX_CERT, (unsigned) FPM_HTTP_TLS_RELOAD_MAX_KEY);
		fpm_http_tls_free(fresh);
		memcpy(r->cert_digest, cert_digest, sizeof(r->cert_digest));
		memcpy(r->key_digest, key_digest, sizeof(r->key_digest));
		return;
	}

	gen = r->shared->generation;
	target = (unsigned) ((gen + 1) % 2);

	r->shared->slot[target].cert_len = fresh->cert_len;
	r->shared->slot[target].key_len = fresh->key_len;
	r->shared->slot[target].min_version = fresh->min_version;
	memcpy(r->shared->slot[target].cert_pem, fresh->cert_pem, fresh->cert_len);
	memcpy(r->shared->slot[target].key_pem, fresh->key_pem, fresh->key_len);
	memcpy(r->shared->slot[target].ticket_key, fresh->ticket_key, sizeof(fresh->ticket_key));

	/* Publish: single writer (this timer), so a plain CAS from the known old
	 * value cannot race with anyone else -- matches the existing
	 * atomic_cmp_set() idiom rather than a bare store. */
	atomic_cmp_set(&r->shared->generation, gen, gen + 1);

	memcpy(r->cert_digest, cert_digest, sizeof(r->cert_digest));
	memcpy(r->key_digest, key_digest, sizeof(r->key_digest));
	fpm_http_tls_free(fresh);

	zlog(ZLOG_NOTICE, "[pool %s] http: TLS certificate reloaded from disk (generation %lu); gateway processes adopt it within http.tls_reload_check seconds",
		r->pool, gen + 1);
}
/* }}} */

struct fpm_http_tls_reload_s *fpm_http_tls_reload_master_init(const char *pool,
	const char *cert_path, const char *key_path, const char *min_version,
	struct fpm_http_tls_s *initial, int check_interval_sec) /* {{{ */
{
	struct fpm_http_tls_reload_s *r;

	if (initial->cert_len > FPM_HTTP_TLS_RELOAD_MAX_CERT || initial->key_len > FPM_HTTP_TLS_RELOAD_MAX_KEY) {
		zlog(ZLOG_WARNING, "[pool %s] http.tls_reload_check: certificate/key is larger than the %u/%u byte reload buffer, disabling reload for this pool (a restart is needed to pick up a renewed certificate)",
			pool, (unsigned) FPM_HTTP_TLS_RELOAD_MAX_CERT, (unsigned) FPM_HTTP_TLS_RELOAD_MAX_KEY);
		return NULL;
	}

	r = calloc(1, sizeof(*r));
	if (!r) {
		return NULL;
	}

	/* fpm_shm_alloc() is anonymous mmap(MAP_SHARED), zero-filled by the
	 * kernel -- no memset needed, generation starts at 0 and slot[0] is
	 * about to be the only slot with real content in it, which is exactly
	 * what generation 0 should mean. */
	r->shared = fpm_shm_alloc(sizeof(*r->shared));
	if (!r->shared) {
		zlog(ZLOG_ERROR, "[pool %s] http.tls_reload_check: cannot allocate shared memory, reload disabled for this pool", pool);
		free(r);
		return NULL;
	}

	r->shared->slot[0].cert_len = initial->cert_len;
	r->shared->slot[0].key_len = initial->key_len;
	r->shared->slot[0].min_version = initial->min_version;
	memcpy(r->shared->slot[0].cert_pem, initial->cert_pem, initial->cert_len);
	memcpy(r->shared->slot[0].key_pem, initial->key_pem, initial->key_len);
	memcpy(r->shared->slot[0].ticket_key, initial->ticket_key, sizeof(initial->ticket_key));
	/* Plain store: nothing has forked yet, so there is no other reader. */
	r->shared->generation = 0;

	r->pool = strdup(pool);
	r->cert_path = strdup(cert_path);
	r->key_path = strdup(key_path);
	r->min_version = min_version && *min_version ? strdup(min_version) : NULL;
	r->check_interval_sec = check_interval_sec;
	/* Borrowed, not copied -- see the fields' declaration above. */
	r->sni = initial->sni;
	r->sni_count = initial->sni_count;

	/* Baseline: the bytes the gateway is about to start serving. A failure
	 * here leaves the digest all-zero, which no file matches, so the first
	 * tick that can read the pair treats it as a change and reloads it --
	 * one redundant load, never a missed one. */
	if (fpm_http_tls_reload_file_digest(cert_path, FPM_HTTP_TLS_RELOAD_MAX_CERT, r->cert_digest) != 0) {
		memset(r->cert_digest, 0, sizeof(r->cert_digest));
	}
	if (fpm_http_tls_reload_file_digest(key_path, FPM_HTTP_TLS_RELOAD_MAX_KEY, r->key_digest) != 0) {
		memset(r->key_digest, 0, sizeof(r->key_digest));
	}

	if (check_interval_sec > 0) {
		fpm_event_set_timer(&r->master_timer, FPM_EV_PERSIST, fpm_http_tls_reload_master_tick, r);
		fpm_event_add(&r->master_timer, (unsigned long) check_interval_sec * 1000);
	}

	return r;
}
/* }}} */

static void fpm_http_tls_reload_child_tick(evutil_socket_t fd, short what, void *arg) /* {{{ */
{
	struct fpm_http_tls_reload_s *r = arg;
	unsigned long gen = r->shared->generation;
	struct fpm_http_tls_reload_slot_s *slot;
	struct fpm_http_tls_s tmp;
	SSL_CTX *new_ctx;

	(void) fd; (void) what;

	if (gen == r->last_seen_generation) {
		return;
	}

	slot = &r->shared->slot[gen % 2];
	/* Copy out of shared memory before use, not a pointer straight into it:
	 * the read is synchronous and single-shot, but copying first (rather
	 * than pointing fpm_http_tls_ctx_new() at the shm slot directly) means
	 * this can never observe a slot the master is mid-write on, no matter
	 * how tight two publishes land back to back. */
	memcpy(r->local_cert_pem, slot->cert_pem, slot->cert_len);
	memcpy(r->local_key_pem, slot->key_pem, slot->key_len);
	memset(&tmp, 0, sizeof(tmp));
	tmp.cert_pem = r->local_cert_pem;
	tmp.cert_len = slot->cert_len;
	tmp.key_pem = r->local_key_pem;
	tmp.key_len = slot->key_len;
	tmp.min_version = slot->min_version;
	memcpy(tmp.ticket_key, slot->ticket_key, sizeof(tmp.ticket_key));
	/* SNI certificates are not part of the reload/mtime-check machinery
	 * (task 041 scope cut, see the fields' declaration above) -- borrow them
	 * from the original, never-freed gw->tls so a hot-reload of the primary
	 * cert does not silently rebuild the ctx with zero SNI certificates. */
	tmp.sni = r->sni;
	tmp.sni_count = r->sni_count;

	new_ctx = fpm_http_tls_ctx_new(r->pool, &tmp);
	if (!new_ctx) {
		/* fpm_http_tls_ctx_new() already logged. These bytes were already
		 * validated once by the master before publishing them, so reaching
		 * this should be impossible -- keep serving the working SSL_CTX and
		 * retry next tick rather than tearing down a working listener. */
		return;
	}

	/* Keep the listener's own bevcb (the gateway's wrapper -- task 031):
	 * it reads *r->child_ctx_slot at connection-accept time, so updating the
	 * slot below is enough for new connections to get the reloaded cert.
	 * NULL means this child predates the wrapper (cannot happen today;
	 * fpm_http.c always passes it), in which case the plain TLS bevcb is
	 * the correct historical fallback. */
	if (r->child_bevcb) {
		evhttp_set_bevcb(r->child_http, r->child_bevcb, r->child_bevcb_arg);
	} else {
		evhttp_set_bevcb(r->child_http, fpm_http_tls_bevcb, new_ctx);
	}
	SSL_CTX_free(*r->child_ctx_slot);
	*r->child_ctx_slot = new_ctx;
	r->last_seen_generation = gen;

	zlog(ZLOG_NOTICE, "[pool %s] http gateway: adopted reloaded TLS certificate (generation %lu)", r->pool, gen);
}
/* }}} */

void fpm_http_tls_reload_child_init(struct fpm_http_tls_reload_s *reload,
	struct event_base *base, struct evhttp *http, SSL_CTX **ctx_slot,
	struct bufferevent *(*bevcb)(struct event_base *, void *), void *bevcb_arg) /* {{{ */
{
	struct timeval every;

	if (!reload || reload->check_interval_sec <= 0) {
		return;
	}

	reload->child_http = http;
	reload->child_ctx_slot = ctx_slot;
	reload->child_bevcb = bevcb;
	reload->child_bevcb_arg = bevcb_arg;	/* Whatever generation *ctx_slot was JUST built from (fpm_http_tls_ctx_new(),
	 * called right before this) -- not 0 -- so this does not immediately
	 * "reload" itself against the exact bytes it just started with. */
	reload->last_seen_generation = reload->shared->generation;

	every.tv_sec = reload->check_interval_sec;
	every.tv_usec = 0;
	reload->child_timer = event_new(base, -1, EV_PERSIST, fpm_http_tls_reload_child_tick, reload);
	event_add(reload->child_timer, &every);
}
/* }}} */

void fpm_http_tls_reload_free(struct fpm_http_tls_reload_s *reload) /* {{{ */
{
	if (!reload) {
		return;
	}
	if (reload->shared) {
		fpm_shm_free((void*) reload->shared, sizeof(*reload->shared));
	}
	free(reload->pool);
	free(reload->cert_path);
	free(reload->key_path);
	free(reload->min_version);
	free(reload);
}
/* }}} */

#endif /* HAVE_FPM_HTTP_TLS */
