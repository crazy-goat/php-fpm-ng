/* fpm-ng: in-process arbiter for ext/session's "files" save handler under
 * pool.executor = fiber — SPIKE. See fpm_pool_coop_session_lock.h and
 * docs/session-lock-arbiter-report.md for the problem statement and the
 * source-reading evidence behind the design decisions here.
 *
 * --- Why this needs a real link dependency on ext/session -----------------
 *
 * Every other hook this project has added around ext/session
 * (fpm_pool_coop_session.c) goes out of its way to avoid a hard link
 * dependency on any ext/session symbol, because ext/session could in
 * principle be built --enable-session=shared, in which case a direct
 * `extern` reference to one of its symbols (a function, or a global like
 * ps_globals) would fail to link into this SAPI binary. That file instead
 * finds ps_globals's address via the mh_arg2 of an ini entry, and the
 * session module's RINIT/RSHUTDOWN via module_registry, both looked up by
 * name/hash at runtime, never by symbol.
 *
 * That trick is NOT available here. Verified against
 * /Users/piotr.halas/work/php-src/ext/session/session.c (read-only):
 * ps_modules[0] is hard-coded to &ps_mod_files forever
 * (session.c:1208-1210), php_session_register_module() can only append into
 * a later free slot (session.c:1213-1224), and _php_find_ps_module()
 * (session.c:1488-1499) is a linear scan from index 0 that returns the
 * FIRST name match — so registering another module also named "files" can
 * never be found; the built-in one always wins. Worse: php_rinit_session()
 * (session.c:2815-2833) sets PS(mod) = NULL and re-resolves it from the
 * CURRENT session.save_handler ini string via _php_find_ps_module() on
 * EVERY request (not once at MINIT) — so there is no live ps_globals.mod
 * pointer we could patch after the fact and have it stick into the next
 * request; the only lever is the ini string itself plus the contents of
 * ps_modules[].
 *
 * Consequently, this file:
 *   - registers a DIFFERENTLY NAMED module, "files_arb", via the real
 *     php_session_register_module() (an exported ext/session PHPAPI
 *     symbol — a genuine, new link-time dependency);
 *   - requires the pool to set `session.save_handler = files_arb` — plain
 *     `session.save_handler = files` gets NO protection from this file;
 *   - fetches the real "files" module's function pointers once via
 *     _php_find_ps_module("files") (same kind of dependency) to delegate to.
 *
 * If ext/session is ever built shared without this object also being linked
 * after it, or if either of these two PHPAPI symbols is ever removed
 * upstream, the SAPI binary fails to link. This project has never built
 * session shared so far — this is a latent, not active, risk — but it is a
 * real one this file introduces that fpm_pool_coop_session.c specifically
 * does not have.
 *
 * --- Locking model ----------------------------------------------------------
 *
 * One process-global HashTable, keyed by the session id (zend_string
 * CONTENT, hash+memcmp — not pointer identity, since the exact zend_string*
 * object backing an id is not guaranteed the same object across the
 * s_read()/s_close() pair). Each entry: held flag, owning ctx pointer, FIFO
 * queue of fpm_pool_fiber_waiter() handles. A second HashTable indexes
 * entries by owning ctx pointer for O(1) force-release from
 * fpm_coop_session_lock_req_free().
 *
 * Locking wraps only s_read (matches mod_files.c: the first ps_files_open()
 * — and hence the first flock() — happens from the s_read call in the
 * normal session_start() path, session.c:480). Release happens in s_close,
 * AFTER delegating to the real close (which is what actually closes the fd
 * and drops the kernel flock) — so a woken in-process waiter always finds
 * the kernel lock already free. s_write/s_destroy/s_gc/s_create_sid/
 * s_validate_sid/s_update_timestamp are plain pass-throughs.
 *
 * Release uses Mesa-style signal-one-and-recheck: waking a waiter does not
 * hand it the lock directly, it just re-enters the `while (held)` loop —
 * a fresh s_read() for the same id, arriving after the release but before
 * the woken fiber resumes, can win first. This does not break mutual
 * exclusion (whoever observes held==false first, synchronously, is the only
 * one who can set it back to true before yielding), it only means ordering
 * under heavy contention is not strict FIFO.
 */

#include "fpm_config.h"

#include "php.h"
#include "zend_hash.h"
#include "ext/session/php_session.h"

#include "fpm_pool_coop.h"
#include "fpm_pool_coop_session_lock.h"
#include "fpm_pool_fiber.h"
#include "zlog.h"

typedef struct arb_waiter_s {
	void *handle;
	struct arb_waiter_s *next;
} arb_waiter;

typedef struct arb_entry_s {
	bool held;
	struct fpm_coop_req_s *owner;
	arb_waiter *waiters_head;
	arb_waiter *waiters_tail;
} arb_entry;

/* mod_data our wrapper installs in *mod_data, wrapping the real module's own
 * mod_data opaquely. */
typedef struct arb_mod_data_s {
	void *inner;
	zend_string *locked_key; /* session id this handle currently holds the
	                          * in-process lock for, or NULL. */
} arb_mod_data;

static bool fpm_coop_session_lock_ready = false;
static const ps_module *fpm_coop_session_lock_orig;
static HashTable fpm_coop_session_lock_table;   /* zend_string(session id) -> arb_entry* */
static HashTable fpm_coop_session_lock_owners;  /* (zend_ulong)(uintptr_t)ctx -> arb_entry* */
static struct fpm_coop_req_s *fpm_coop_session_lock_current_ctx;
static bool fpm_coop_session_lock_nowait_warned = false;

static void arb_entry_val_dtor(zval *pDest) /* {{{ */
{
	arb_entry *e = (arb_entry *) Z_PTR_P(pDest);
	arb_waiter *w = e->waiters_head;

	while (w) {
		arb_waiter *next = w->next;
		efree(w);
		w = next;
	}
	efree(e);
}
/* }}} */

static void arb_waiter_enqueue(arb_entry *e, void *handle) /* {{{ */
{
	arb_waiter *w = emalloc(sizeof(*w));

	w->handle = handle;
	w->next = NULL;
	if (e->waiters_tail) {
		e->waiters_tail->next = w;
	} else {
		e->waiters_head = w;
	}
	e->waiters_tail = w;
}
/* }}} */

/* Wakes at most one waiter for e, if any. Does not touch e->held (caller's
 * job) and does not delete e even if now idle (caller decides). Returns
 * true iff a waiter was actually woken - see the BUGFIX note on the call
 * site in arb_lock_release() for why the caller must not delete e in that
 * case. */
static bool arb_wake_one(arb_entry *e) /* {{{ */
{
	arb_waiter *w = e->waiters_head;

	if (!w) {
		return false;
	}
	e->waiters_head = w->next;
	if (!e->waiters_head) {
		e->waiters_tail = NULL;
	}
	fpm_pool_fiber_wake(w->handle);
	efree(w);
	return true;
}
/* }}} */

static void arb_set_owner(arb_entry *e, struct fpm_coop_req_s *ctx) /* {{{ */
{
	e->owner = ctx;
	if (ctx) {
		zend_hash_index_update_ptr(&fpm_coop_session_lock_owners, (zend_ulong)(uintptr_t) ctx, e);
	}
}
/* }}} */

static void arb_clear_owner(arb_entry *e) /* {{{ */
{
	if (e->owner) {
		zend_hash_index_del(&fpm_coop_session_lock_owners, (zend_ulong)(uintptr_t) e->owner);
		e->owner = NULL;
	}
}
/* }}} */

/* Returns true if the in-process lock for key is now held by us (either
 * uncontended, or after waiting it out). Returns false only when it could
 * not even attempt to wait (fpm_pool_fiber_can_wait() says no — e.g. a user
 * script's own nested Fiber calling session_start() from inside it): in that
 * narrow case we log once and the caller proceeds WITHOUT the in-process
 * lock, i.e. the original raw-flock deadlock risk still exists there. */
static bool arb_lock_acquire(zend_string *key) /* {{{ */
{
	arb_entry *e = zend_hash_find_ptr(&fpm_coop_session_lock_table, key);

	if (!e) {
		e = ecalloc(1, sizeof(*e));
		zend_hash_update_ptr(&fpm_coop_session_lock_table, key, e);
	}

	while (e->held) {
		void *waiter;

		if (!fpm_pool_fiber_can_wait()) {
			if (!fpm_coop_session_lock_nowait_warned) {
				fpm_coop_session_lock_nowait_warned = true;
				zlog(ZLOG_WARNING, "[pool %s] coop-session-lock: cannot suspend the current fiber to "
					"wait for an in-process session lock (nested user Fiber?) - proceeding without it; "
					"the raw flock() deadlock this feature exists to remove is still possible in this "
					"specific case (logged once per process)", fpm_coop_pool_name());
			}
			return false;
		}

		waiter = fpm_pool_fiber_waiter();
		arb_waiter_enqueue(e, waiter);
		fpm_pool_fiber_wait_wake(NULL);
		/* Woken (or, in principle, spuriously) - loop rechecks e->held. */
	}

	e->held = true;
	arb_set_owner(e, fpm_coop_session_lock_current_ctx);
	return true;
}
/* }}} */

static void arb_lock_release(zend_string *key) /* {{{ */
{
	arb_entry *e = zend_hash_find_ptr(&fpm_coop_session_lock_table, key);

	if (!e) {
		/* Should not happen: locked_key is only ever set right after a
		 * successful arb_lock_acquire(), which always leaves an entry. */
		return;
	}

	e->held = false;
	arb_clear_owner(e);

	/* BUGFIX (measured as a live SIGSEGV during this spike's E1 run - see
	 * "## Bug found and fixed during measurement" in
	 * docs/session-lock-arbiter-report.md for the captured backtrace):
	 * arb_wake_one() only marks a waiter's fiber runnable, it does not run
	 * it synchronously - the woken fiber does not actually resume until the
	 * scheduler gets back to it, later in this same process's event loop,
	 * and when it does resume it re-enters arb_lock_acquire()'s `while
	 * (e->held)` loop and dereferences this SAME `e` again. If we deleted
	 * (and efree()d) `e` here just because its waiter queue is empty RIGHT
	 * NOW - which it always is immediately after dequeuing the one waiter
	 * we just woke - that woken fiber resumes holding a dangling pointer:
	 * a textbook use-after-free, and the freed 32 bytes are typically
	 * reused for something else by the time it resumes (observed: the
	 * request in flight at release time does its own RSHUTDOWN/response
	 * teardown allocations first), so the woken fiber reads garbage out of
	 * where e->waiters_tail used to be and crashes trying to enqueue itself
	 * behind it. Fix: only ever delete e when we did NOT just hand off to a
	 * waiter, i.e. when e was genuinely idle with nobody left to touch it -
	 * this does not change locking/ordering semantics at all, it only
	 * changes when the bookkeeping struct's memory is reclaimed. */
	if (!arb_wake_one(e) && !e->waiters_head) {
		/* Nobody holding, nobody waiting, nobody just woken: bound memory,
		 * delete now. A later s_read() for the same id just creates a
		 * fresh entry. */
		zend_hash_del(&fpm_coop_session_lock_table, key);
	}
}
/* }}} */

static zend_result arb_open(PS_OPEN_ARGS) /* {{{ */
{
	arb_mod_data *d = ecalloc(1, sizeof(*d));
	zend_result ret;

	*mod_data = d;
	ret = fpm_coop_session_lock_orig->s_open(&d->inner, save_path, session_name);
	return ret;
}
/* }}} */

static zend_result arb_close(PS_CLOSE_ARGS) /* {{{ */
{
	arb_mod_data *d = *mod_data;
	zend_result ret;

	if (!d) {
		return SUCCESS;
	}

	/* Real close() first - this is what actually drops the kernel flock -
	 * THEN release our in-process lock, so a woken in-process waiter never
	 * finds the kernel lock still held. */
	ret = fpm_coop_session_lock_orig->s_close(&d->inner);

	if (d->locked_key) {
		arb_lock_release(d->locked_key);
		zend_string_release_ex(d->locked_key, false);
		d->locked_key = NULL;
	}

	efree(d);
	*mod_data = NULL;
	return ret;
}
/* }}} */

static zend_result arb_read(PS_READ_ARGS) /* {{{ */
{
	arb_mod_data *d = *mod_data;

	if (!d) {
		/* Defensive: s_open() is always called first by ext/session
		 * (session.c: php_session_initialize()) before s_read(); we should
		 * never actually get here with no wrapper mod_data. */
		return FAILURE;
	}

	if (arb_lock_acquire(key) && !d->locked_key) {
		d->locked_key = zend_string_copy(key);
	}

	return fpm_coop_session_lock_orig->s_read(&d->inner, key, val, maxlifetime);
}
/* }}} */

static zend_result arb_write(PS_WRITE_ARGS) /* {{{ */
{
	arb_mod_data *d = *mod_data;

	if (!d) {
		return FAILURE;
	}
	return fpm_coop_session_lock_orig->s_write(&d->inner, key, val, maxlifetime);
}
/* }}} */

static zend_result arb_destroy(PS_DESTROY_ARGS) /* {{{ */
{
	arb_mod_data *d = *mod_data;

	if (!d) {
		return FAILURE;
	}
	return fpm_coop_session_lock_orig->s_destroy(&d->inner, key);
}
/* }}} */

static zend_long arb_gc(PS_GC_ARGS) /* {{{ */
{
	arb_mod_data *d = *mod_data;

	if (!d) {
		*nrdels = 0;
		return 0;
	}
	return fpm_coop_session_lock_orig->s_gc(&d->inner, maxlifetime, nrdels);
}
/* }}} */

static zend_string *arb_create_sid(PS_CREATE_SID_ARGS) /* {{{ */
{
	arb_mod_data *d = *mod_data;

	if (!d) {
		return NULL;
	}
	return fpm_coop_session_lock_orig->s_create_sid(&d->inner);
}
/* }}} */

static zend_result arb_validate_sid(PS_VALIDATE_SID_ARGS) /* {{{ */
{
	arb_mod_data *d = *mod_data;

	if (!d) {
		return FAILURE;
	}
	return fpm_coop_session_lock_orig->s_validate_sid(&d->inner, key);
}
/* }}} */

static zend_result arb_update_timestamp(PS_UPDATE_TIMESTAMP_ARGS) /* {{{ */
{
	arb_mod_data *d = *mod_data;

	if (!d) {
		return FAILURE;
	}
	return fpm_coop_session_lock_orig->s_update_timestamp(&d->inner, key, val, maxlifetime);
}
/* }}} */

static const ps_module fpm_coop_session_lock_mod = {
	"files_arb",
	arb_open, arb_close, arb_read, arb_write,
	arb_destroy, arb_gc, arb_create_sid,
	arb_validate_sid, arb_update_timestamp
};

void fpm_coop_session_lock_container_start(void) /* {{{ */
{
	if (zend_get_module_started("session") != SUCCESS) {
		return;
	}

	fpm_coop_session_lock_orig = _php_find_ps_module("files");
	if (!fpm_coop_session_lock_orig) {
		zlog(ZLOG_WARNING, "[pool %s] coop-session-lock: no 'files' save handler module found - "
			"in-process session-lock arbiter NOT installed", fpm_coop_pool_name());
		return;
	}

	if (php_session_register_module(&fpm_coop_session_lock_mod) != SUCCESS) {
		zlog(ZLOG_WARNING, "[pool %s] coop-session-lock: php_session_register_module('files_arb') failed "
			"(module table full?) - in-process session-lock arbiter NOT installed", fpm_coop_pool_name());
		fpm_coop_session_lock_orig = NULL;
		return;
	}

	zend_hash_init(&fpm_coop_session_lock_table, 8, NULL, arb_entry_val_dtor, 0);
	zend_hash_init(&fpm_coop_session_lock_owners, 8, NULL, NULL, 0);
	fpm_coop_session_lock_ready = true;

	zlog(ZLOG_NOTICE, "[pool %s] coop-session-lock: in-process session-lock arbiter installed as save "
		"handler 'files_arb' (delegates to 'files'); set session.save_handler = files_arb to use it - "
		"plain session.save_handler = files is NOT overridden and gets no protection from this feature",
		fpm_coop_pool_name());
}
/* }}} */

void fpm_coop_session_lock_req_enter(struct fpm_coop_req_s *ctx) /* {{{ */
{
	if (!fpm_coop_session_lock_ready) {
		return;
	}
	fpm_coop_session_lock_current_ctx = ctx;
}
/* }}} */

void fpm_coop_session_lock_req_free(struct fpm_coop_req_s *ctx) /* {{{ */
{
	arb_entry *e;

	if (!fpm_coop_session_lock_ready) {
		return;
	}

	e = zend_hash_index_find_ptr(&fpm_coop_session_lock_owners, (zend_ulong)(uintptr_t) ctx);
	if (!e) {
		return;
	}

	zlog(ZLOG_WARNING, "[pool %s] coop-session-lock: request ctx #%u freed while still attributed as the "
		"holder of an in-process session lock - force-releasing it (leak safety net). Note the real "
		"kernel-level flock/fd for this request's session may ALSO leak if RSHUTDOWN did not run - a "
		"pre-existing risk shared with the unmodified coop-session code, not introduced by this feature",
		fpm_coop_pool_name(), ctx->id);

	e->held = false;
	arb_clear_owner(e);
	arb_wake_one(e);
	/* Leave the entry in the table even with no waiters now - harmless, a
	 * later s_read() for the same id will just find held == false. */
}
/* }}} */
