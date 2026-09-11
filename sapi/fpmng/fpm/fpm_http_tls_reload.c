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
	/* Master only: has the "certificate has become unreadable" warning
	 * already been logged for the current disappearance? Cleared as soon as
	 * the pair reads again, so a second disappearance warns again. */
	int warned_unreadable;
	/* NO_CERT -> READY hook, issue #172; NULL for every pool that started
	 * with a certificate, which is all of them unless
	 * http.tls_wait_for_cert is set. Cleared as it fires so it cannot run
	 * twice. */
	void (*child_on_first_cert)(void *);
	void *child_on_first_cert_arg;
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
	int first_cert;

	(void) ev;
	if (which != FPM_EV_TIMEOUT) {
		return;
	}

	if (fpm_http_tls_reload_file_digest(r->cert_path, FPM_HTTP_TLS_RELOAD_MAX_CERT, cert_digest) != 0 ||
			fpm_http_tls_reload_file_digest(r->key_path, FPM_HTTP_TLS_RELOAD_MAX_KEY, key_digest) != 0) {
		/* Transient (mid write, or the volume is not there yet): not fatal,
		 * skip this tick, the certificate already in use keeps serving. */
		if (!r->warned_unreadable && fpm_http_tls_reload_has_cert(r)) {
			/* Once, and only for a pool that IS serving a certificate. This
			 * is the answer to issue #172 criterion 5's "what does the
			 * operator see instead": deleting the certificate under a live
			 * pool deliberately does NOT drop it back to NO_CERT -- an
			 * operator mistake or a half-finished write must not take TLS
			 * down -- so without a line here the only symptom would be the
			 * certificate silently expiring weeks later. Warning rather than
			 * error: nothing is broken yet, and the pool recovers by itself
			 * the moment the pair is readable again.
			 *
			 * Not logged in NO_CERT, where an unreadable pair is the normal
			 * state and this would fire on every tick of every first boot. */
			zlog(ZLOG_WARNING, "[pool %s] http: TLS certificate/key at '%s' has become unreadable; the certificate already loaded keeps serving and will NOT be renewed until the pair is back",
				r->pool, r->cert_path);
			r->warned_unreadable = 1;
		}
		return;
	}
	if (r->warned_unreadable) {
		zlog(ZLOG_NOTICE, "[pool %s] http: TLS certificate/key at '%s' is readable again", r->pool, r->cert_path);
		r->warned_unreadable = 0;
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
	/* Read before the publish below overwrites nothing relevant but the
	 * generation: an empty current slot means this pool has been in NO_CERT
	 * (issue #172) and this is its first certificate, not a renewal. The two
	 * deserve different log lines -- "reloaded" on a first boot would be a
	 * lie, and the transition is the line an operator greps for. */
	first_cert = r->shared->slot[gen % 2].cert_len == 0;

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

	if (first_cert) {
		zlog(ZLOG_NOTICE, "[pool %s] http: TLS certificate found on disk (generation %lu); the pool leaves NO_CERT and each gateway process opens its TLS listener within http.tls_reload_check seconds",
			r->pool, gen + 1);
	} else {
		zlog(ZLOG_NOTICE, "[pool %s] http: TLS certificate reloaded from disk (generation %lu); gateway processes adopt it within http.tls_reload_check seconds",
			r->pool, gen + 1);
	}
}
/* }}} */

struct fpm_http_tls_reload_s *fpm_http_tls_reload_master_init(const char *pool,
	const char *cert_path, const char *key_path, const char *min_version,
	struct fpm_http_tls_s *initial, int check_interval_sec) /* {{{ */
{
	struct fpm_http_tls_reload_s *r;

	if (initial && (initial->cert_len > FPM_HTTP_TLS_RELOAD_MAX_CERT || initial->key_len > FPM_HTTP_TLS_RELOAD_MAX_KEY)) {
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

	/* NULL `initial` is http.tls_wait_for_cert's NO_CERT state (issue #172):
	 * leave generation 0 exactly as fpm_shm_alloc() returned it -- all zero,
	 * so cert_len is 0 and fpm_http_tls_reload_has_cert() below reads false.
	 * Generation 0 still exists and is still the published one; it just says
	 * "no certificate yet" rather than "here is the startup certificate". */
	if (initial) {
		r->shared->slot[0].cert_len = initial->cert_len;
		r->shared->slot[0].key_len = initial->key_len;
		r->shared->slot[0].min_version = initial->min_version;
		memcpy(r->shared->slot[0].cert_pem, initial->cert_pem, initial->cert_len);
		memcpy(r->shared->slot[0].key_pem, initial->key_pem, initial->key_len);
		memcpy(r->shared->slot[0].ticket_key, initial->ticket_key, sizeof(initial->ticket_key));
	}
	/* Plain store: nothing has forked yet, so there is no other reader. */
	r->shared->generation = 0;

	r->pool = strdup(pool);
	r->cert_path = strdup(cert_path);
	r->key_path = strdup(key_path);
	r->min_version = min_version && *min_version ? strdup(min_version) : NULL;
	r->check_interval_sec = check_interval_sec;
	/* Borrowed, not copied -- see the fields' declaration above. */
	r->sni = initial ? initial->sni : NULL;
	r->sni_count = initial ? initial->sni_count : 0;

	/* Baseline: the bytes the gateway is about to start serving. A failure
	 * here leaves the digest all-zero, which no file matches, so the first
	 * tick that can read the pair treats it as a change and reloads it --
	 * one redundant load, never a missed one. */
	if (!initial) {
		/* NO_CERT: no baseline at all. Taking one here would be actively
		 * wrong -- a certificate already sitting on disk (a restart of a pool
		 * that is past its first issuance, or a write that landed between
		 * fpm_http_tls_load() failing and this call) would become the
		 * "nothing has changed" reference and never be published, leaving the
		 * pool in NO_CERT forever with the certificate right there. All-zero
		 * matches no file, so the first tick that can read the pair publishes
		 * it. */
		memset(r->cert_digest, 0, sizeof(r->cert_digest));
		memset(r->key_digest, 0, sizeof(r->key_digest));
	} else {
		if (fpm_http_tls_reload_file_digest(cert_path, FPM_HTTP_TLS_RELOAD_MAX_CERT, r->cert_digest) != 0) {
			memset(r->cert_digest, 0, sizeof(r->cert_digest));
		}
		if (fpm_http_tls_reload_file_digest(key_path, FPM_HTTP_TLS_RELOAD_MAX_KEY, r->key_digest) != 0) {
			memset(r->key_digest, 0, sizeof(r->key_digest));
		}
	}

	if (check_interval_sec > 0) {
		fpm_event_set_timer(&r->master_timer, FPM_EV_PERSIST, fpm_http_tls_reload_master_tick, r);
		fpm_event_add(&r->master_timer, (unsigned long) check_interval_sec * 1000);
	}

	return r;
}
/* }}} */

/* Copies the slot currently published in shared memory into `tmp` and returns
 * the generation it came from. `tmp`'s cert_pem/key_pem point at this child's
 * own private local_*_pem buffers afterwards, so `tmp` is only usable while
 * `r` is, and only by the one child that owns it.
 *
 * Copying out of shared memory before use, rather than pointing
 * fpm_http_tls_ctx_new() at the shm slot directly, gives the caller a stable
 * DESTINATION. It does not on its own give a stable SOURCE: there are only two
 * slots, so a publish landing two generations ahead writes into the very slot
 * being copied. Hence the generation is latched before the copy and re-read
 * after it, and a copy that straddled a publish is redone.
 *
 * Bounded rather than unbounded: the master publishes at most once per
 * http.tls_reload_check seconds (>= 1) while the copy is at most 80 KB, so
 * even one retry is already unreachable in practice -- a spinning loop here
 * would only be a way for a broken master to hang a gateway's event loop. On
 * exhaustion this returns the generation it last latched and the copy may be
 * torn, which is self-healing: a torn PEM fails fpm_http_tls_ctx_new(), so the
 * caller either falls back to gw->tls (startup) or keeps the working SSL_CTX
 * and retries on the next tick, in both cases leaving last_seen_generation
 * alone. */
#define FPM_HTTP_TLS_RELOAD_SNAPSHOT_TRIES 4

static unsigned long fpm_http_tls_reload_snapshot(struct fpm_http_tls_reload_s *r,
	struct fpm_http_tls_s *tmp) /* {{{ */
{
	unsigned long gen;
	struct fpm_http_tls_reload_slot_s *slot;
	int tries = FPM_HTTP_TLS_RELOAD_SNAPSHOT_TRIES;

	memset(tmp, 0, sizeof(*tmp));
	tmp->cert_pem = r->local_cert_pem;
	tmp->key_pem = r->local_key_pem;

	/* Every field is taken inside the loop, not re-read from the slot after
	 * it: a length or a min_version read after the copy could describe a
	 * different generation than the bytes that were copied. */
	do {
		gen = r->shared->generation;
		slot = &r->shared->slot[gen % 2];
		tmp->cert_len = slot->cert_len;
		tmp->key_len = slot->key_len;
		tmp->min_version = slot->min_version;
		/* Clamped because these two lengths come out of shared memory and
		 * drive a memcpy into fixed-size buffers. The master never publishes
		 * more (both the startup pair and every candidate are checked against
		 * MAX_CERT/MAX_KEY before they reach a slot), so this can only ever
		 * fire on a corrupted region -- and then it truncates into a PEM that
		 * fails to parse instead of overrunning local_cert_pem. */
		if (tmp->cert_len > sizeof(r->local_cert_pem)) {
			tmp->cert_len = sizeof(r->local_cert_pem);
		}
		if (tmp->key_len > sizeof(r->local_key_pem)) {
			tmp->key_len = sizeof(r->local_key_pem);
		}
		memcpy(r->local_cert_pem, slot->cert_pem, tmp->cert_len);
		memcpy(r->local_key_pem, slot->key_pem, tmp->key_len);
		memcpy(tmp->ticket_key, slot->ticket_key, sizeof(tmp->ticket_key));
	} while (gen != r->shared->generation && --tries > 0);

	/* SNI certificates are not part of the reload/mtime-check machinery
	 * (task 041 scope cut, see the fields' declaration above) -- borrow them
	 * from the original, never-freed gw->tls so a hot-reload of the primary
	 * cert does not silently rebuild the ctx with zero SNI certificates. */
	tmp->sni = r->sni;
	tmp->sni_count = r->sni_count;

	return gen;
}
/* }}} */

int fpm_http_tls_reload_has_cert(struct fpm_http_tls_reload_s *reload) /* {{{ */
{
	unsigned long gen;

	if (!reload) {
		return 0;
	}
	gen = reload->shared->generation;
	return reload->shared->slot[gen % 2].cert_len > 0;
}
/* }}} */

SSL_CTX *fpm_http_tls_reload_child_ctx_new(struct fpm_http_tls_reload_s *reload) /* {{{ */
{
	struct fpm_http_tls_s tmp;
	unsigned long gen;
	SSL_CTX *ctx;

	if (!reload) {
		return NULL;
	}
	if (!fpm_http_tls_reload_has_cert(reload)) {
		/* NO_CERT (issue #172): return NULL WITHOUT going through
		 * fpm_http_tls_ctx_new(), which would log a parse failure for an
		 * empty PEM on every gateway at every boot. "Not issued yet" is the
		 * configured state here, not an error, and the caller knows the
		 * difference because it is the one that set http.tls_wait_for_cert. */
		return NULL;
	}

	gen = fpm_http_tls_reload_snapshot(reload, &tmp);
	ctx = fpm_http_tls_ctx_new(reload->pool, &tmp);
	if (!ctx) {
		/* fpm_http_tls_ctx_new() already logged. The caller falls back to
		 * gw->tls, which is generation 0's bytes -- and last_seen_generation
		 * is left at its calloc() zero, which is precisely the generation
		 * that fallback ctx would then hold, so the first child tick adopts
		 * whatever is published now. */
		return NULL;
	}

	/* The ctx really was built from `gen`, so record it as adopted: this is
	 * what keeps a gateway forked at startup from logging a spurious
	 * adoption notice for the bytes it just started with, AND what makes a
	 * gateway respawned after N reloads (issue #91) not claim generation N
	 * while holding generation 0's certificate. */
	reload->last_seen_generation = gen;
	return ctx;
}
/* }}} */

static void fpm_http_tls_reload_child_tick(evutil_socket_t fd, short what, void *arg) /* {{{ */
{
	struct fpm_http_tls_reload_s *r = arg;
	unsigned long gen = r->shared->generation;
	struct fpm_http_tls_s tmp;
	SSL_CTX *new_ctx;

	(void) fd; (void) what;

	if (gen == r->last_seen_generation) {
		return;
	}

	gen = fpm_http_tls_reload_snapshot(r, &tmp);

	if (tmp.cert_len == 0) {
		/* Still NO_CERT (issue #172). Unreachable as the master publishes
		 * today -- it only ever publishes a pair it has just validated -- so
		 * this is a guard against a future publisher, not a state the code
		 * below could otherwise be reached in. Leaving last_seen_generation
		 * alone means the next real certificate is still seen as a change. */
		return;
	}

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
	/* SSL_CTX_free(NULL) is a documented no-op, which is what makes the
	 * NO_CERT case (issue #172) need no branch here: this child simply had
	 * no context to free. */
	SSL_CTX_free(*r->child_ctx_slot);
	*r->child_ctx_slot = new_ctx;
	r->last_seen_generation = gen;

	if (r->child_on_first_cert) {
		void (*cb)(void *) = r->child_on_first_cert;
		void *cb_arg = r->child_on_first_cert_arg;

		/* Cleared BEFORE the call, not after: the callback opens this
		 * child's TLS listener and must run exactly once even if it somehow
		 * re-enters this tick. Afterwards this pool is indistinguishable
		 * from one that started with a certificate. */
		r->child_on_first_cert = NULL;
		r->child_on_first_cert_arg = NULL;
		zlog(ZLOG_NOTICE, "[pool %s] http gateway: TLS certificate arrived (generation %lu), leaving NO_CERT", r->pool, gen);
		cb(cb_arg);
		return;
	}

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
	reload->child_bevcb_arg = bevcb_arg;

	/* last_seen_generation is deliberately NOT set here. It must name the
	 * generation *ctx_slot was actually built from, and only the code that
	 * built it knows that: fpm_http_tls_reload_child_ctx_new() above sets it,
	 * and a fallback build from gw->tls leaves it at calloc()'s zero, which is
	 * generation 0 -- what gw->tls holds by definition.
	 *
	 * Seeding it from the CURRENT generation here, as this used to, was issue
	 * #91: correct for a gateway forked at startup (nothing has been reloaded
	 * yet, so the current generation IS the one it was born with), silently
	 * wrong for one respawned after N reloads -- it claimed to have adopted
	 * generation N while serving the startup certificate, so the tick below
	 * returned early for the rest of that process's life. Measured before the
	 * fix: 7 of 24 sampled connections served the previous certificate after
	 * one gateway of three was killed. */

	every.tv_sec = reload->check_interval_sec;
	every.tv_usec = 0;
	reload->child_timer = event_new(base, -1, EV_PERSIST, fpm_http_tls_reload_child_tick, reload);
	event_add(reload->child_timer, &every);
}
/* }}} */

void fpm_http_tls_reload_child_on_first_cert(struct fpm_http_tls_reload_s *reload,
	void (*cb)(void *), void *arg) /* {{{ */
{
	if (!reload) {
		return;
	}
	reload->child_on_first_cert = cb;
	reload->child_on_first_cert_arg = arg;
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
