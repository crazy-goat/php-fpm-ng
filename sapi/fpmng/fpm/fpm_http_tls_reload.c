/* fpm-ng: reload the HTTP gateway's TLS certificate/key without restarting
 * any gateway process, see fpm_http_tls_reload.h. Design (task 040, full
 * reasoning in tasks/040-tls-certificate-reload-without-restart.md):
 *
 * - The master owns the file reads, same property fpm_http_tls.h already
 *   documents for startup -- children never open the key file, here or
 *   anywhere else. A self-rearming timer in the master (fpm_events.c's own
 *   epoll-based loop, the same one fpm_pctl_heartbeat() uses) stat()s
 *   cert_path/key_path every http.tls_reload_check seconds. Never stat() in
 *   the request path.
 * - On an mtime change the master calls the existing, already-stateless
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

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

	/* master-only: last mtimes this timer acted on, and the timer itself */
	time_t cert_mtime;
	time_t key_mtime;
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
	char local_cert_pem[FPM_HTTP_TLS_RELOAD_MAX_CERT];
	char local_key_pem[FPM_HTTP_TLS_RELOAD_MAX_KEY];
};

static void fpm_http_tls_reload_master_tick(struct fpm_event_s *ev, short which, void *arg) /* {{{ */
{
	struct fpm_http_tls_reload_s *r = arg;
	struct stat cert_st, key_st;
	struct fpm_http_tls_s *fresh;
	unsigned long gen;
	unsigned target;

	(void) ev;
	if (which != FPM_EV_TIMEOUT) {
		return;
	}

	if (stat(r->cert_path, &cert_st) != 0 || stat(r->key_path, &key_st) != 0) {
		/* Transient (mid write, or the volume is not there yet): not fatal,
		 * skip this tick, the certificate already in use keeps serving. */
		return;
	}
	if (cert_st.st_mtime == r->cert_mtime && key_st.st_mtime == r->key_mtime) {
		return;
	}

	/* Torn reads, tasks/040 decision: enforced, not just documented. Reuses
	 * the exact check config-validation already runs at startup -- never
	 * logs key material. */
	/* sni_spec: NULL -- this tick only ever re-checks http.tls_cert/
	 * http.tls_key's mtimes (task 040's original scope), never
	 * http.tls_sni_cert's paths (task 041's scope cut, see
	 * fpm_http_tls_reload_s.sni/sni_count below): SNI certificates are not
	 * hot-reloaded, only the primary cert/key are. */
	if (fpm_http_tls_validate(r->pool, r->cert_path, r->key_path, r->min_version, NULL) != 0) {
		/* fpm_http_tls_validate() already logged what's wrong. Remember
		 * these mtimes anyway so a persistently broken pair (operator
		 * hasn't fixed it yet) does not re-log every tick; the next actual
		 * change re-triggers validation. */
		r->cert_mtime = cert_st.st_mtime;
		r->key_mtime = key_st.st_mtime;
		return;
	}

	fresh = fpm_http_tls_load(r->pool, r->cert_path, r->key_path, r->min_version, NULL);
	if (!fresh) {
		/* fpm_http_tls_load() already logged (re-read failed between the
		 * validate above and here, or RAND_bytes() failed) -- do not update
		 * the mtimes, so this is retried next tick rather than skipped. */
		return;
	}

	if (fresh->cert_len > FPM_HTTP_TLS_RELOAD_MAX_CERT || fresh->key_len > FPM_HTTP_TLS_RELOAD_MAX_KEY) {
		zlog(ZLOG_ERROR, "[pool %s] http.tls_reload_check: new certificate/key is larger than the %u/%u byte reload buffer, keeping the certificate already in use",
			r->pool, (unsigned) FPM_HTTP_TLS_RELOAD_MAX_CERT, (unsigned) FPM_HTTP_TLS_RELOAD_MAX_KEY);
		fpm_http_tls_free(fresh);
		r->cert_mtime = cert_st.st_mtime;
		r->key_mtime = key_st.st_mtime;
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

	r->cert_mtime = cert_st.st_mtime;
	r->key_mtime = key_st.st_mtime;
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
	struct stat st;

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

	if (stat(cert_path, &st) == 0) {
		r->cert_mtime = st.st_mtime;
	}
	if (stat(key_path, &st) == 0) {
		r->key_mtime = st.st_mtime;
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

	evhttp_set_bevcb(r->child_http, fpm_http_tls_bevcb, new_ctx);
	SSL_CTX_free(*r->child_ctx_slot);
	*r->child_ctx_slot = new_ctx;
	r->last_seen_generation = gen;

	zlog(ZLOG_NOTICE, "[pool %s] http gateway: adopted reloaded TLS certificate (generation %lu)", r->pool, gen);
}
/* }}} */

void fpm_http_tls_reload_child_init(struct fpm_http_tls_reload_s *reload,
	struct event_base *base, struct evhttp *http, SSL_CTX **ctx_slot) /* {{{ */
{
	struct timeval every;

	if (!reload || reload->check_interval_sec <= 0) {
		return;
	}

	reload->child_http = http;
	reload->child_ctx_slot = ctx_slot;
	/* Whatever generation *ctx_slot was JUST built from (fpm_http_tls_ctx_new(),
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
