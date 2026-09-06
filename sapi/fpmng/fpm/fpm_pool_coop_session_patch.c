/* fpm-ng: SECOND variant of the in-process session-lock arbiter — patches
 * ps_globals.mod in place. See fpm_pool_coop_session_patch.h for the
 * contract and docs/session-lock-arbiter-report.md for the comparison
 * against the "files_arb" variant (fpm_pool_coop_session_lock.c).
 *
 * --- Why this avoids the files_arb variant's link dependency ---------------
 *
 * files_arb needs php_session_register_module()/_php_find_ps_module() (real
 * exported ext/session symbols) because ext/session's own RINIT
 * (php_rinit_session(), session.c:2815-2833) is the ONLY place PS(mod) is
 * ever resolved from the ini string, and it does so via
 * _php_find_ps_module() every single request — the only way to make that
 * lookup return something of ours is to be IN the table it searches.
 *
 * This variant sidesteps that lookup entirely: ps_globals.mod
 * (`const ps_module *mod;`, php_session.h) is an ordinary struct field —
 * the `const` qualifies what it points AT, not the field itself, so the
 * pointer is freely writable. We get ps_globals's address with the exact
 * same no-link-dependency trick fpm_pool_coop_session.c already uses (the
 * mh_arg2 of the "session.save_path" ini entry), then simply overwrite the
 * `mod` field AFTER letting the real RINIT resolve it, right before control
 * returns to the script. This needs zero ext/session symbols: just
 * `zend_ini_string_literal()` (Zend core) to read the current
 * session.save_handler value, and pointer/struct-field reads/writes through
 * an address we computed ourselves.
 *
 * --- Identifying the built-in "files" module without a linker symbol ------
 *
 * We cannot write `extern const ps_module ps_mod_files;` without recreating
 * exactly the link dependency this variant exists to avoid. Instead:
 * fpm_coop_session_patch_container_start() reads whatever ps_globals.mod
 * currently contains at a moment BEFORE this process has ever run RINIT for
 * a real request (see the .h file - called before the container's own
 * php_request_startup()). At that point in a process's life, ps_globals.mod
 * can only hold whatever OnUpdateSaveHandler wrote at MINIT/ini-registration
 * time for the CONFIGURED default (verified: RINIT is the only thing that
 * ever resets it to NULL, and RINIT hasn't run yet) - so if the pool's
 * default session.save_handler is "files", this is guaranteed to be the
 * real, built-in ps_mod_files, not a look-alike: nothing else in the module
 * table could have been substituted in without RINIT (or an explicit
 * ini_set(), which also can't have happened yet) running first. We confirm
 * this defensively by comparing its s_name to the literal "files" purely as
 * a sanity check (belt and suspenders against some unexpected extension
 * order), not as the actual identification mechanism - the identification
 * mechanism is the TIMING (before-first-RINIT), which is what makes this
 * safe against a user module also happening to be named "files": such a
 * module could only ever become reachable via _php_find_ps_module(), which
 * has not run yet at this point.
 *
 * fpm_coop_session_patch_req_apply() (called after each request's REAL
 * RINIT) then re-checks per request: current session.save_handler ini
 * string == "files" (case-insensitively, mirroring exactly the comparison
 * php_rinit_session() itself just used to resolve PS(mod)) AND
 * ps_globals.mod != our own wrapper struct's address already. Only then do
 * we overwrite the field. This is the safe, per-request re-identification:
 * we are not guessing at ps_globals.mod's IDENTITY at that point, we are
 * re-deriving, ourselves, independently, the exact same ini-string-based
 * decision RINIT already made a moment earlier - if the ini string says
 * "files", RINIT's _php_find_ps_module("files") is GUARANTEED (by the
 * ps_modules[] layout established in fpm_pool_coop_session_lock.c's header
 * comment) to have returned the one true built-in module, so there is
 * nothing left to misidentify.
 *
 * --- The auto_start gap (real, structural, not fixed by this variant) -----
 *
 * php_rinit_session() (session.c:2840-2842) calls php_session_start()
 * ITSELF, synchronously, when session.auto_start = 1 - BEFORE returning
 * control to fpm_coop_session_request_startup(), and therefore before
 * fpm_coop_session_patch_req_apply() (called right after that returns) ever
 * runs. For a request with auto_start = 1, the FIRST session_start() call -
 * the one that does the first s_read()/flock() - is NOT protected by this
 * variant: it runs through the real, unwrapped "files" module. Any
 * SUBSEQUENT session_start() in the same request (after a
 * session_write_close()) IS protected, because by then our post-RINIT patch
 * has already run. The files_arb variant does NOT have this gap: it wins
 * RINIT's own _php_find_ps_module() lookup directly, so even the
 * auto_start-triggered session_start() inside RINIT already uses the
 * wrapper. This is the single biggest correctness trade-off between the two
 * variants and is measured explicitly in the report.
 *
 * --- ini_set() mid-request: detect, don't silently lose protection --------
 *
 * ext/session's OnUpdateSaveHandler ini callback (session.c:582-611) writes
 * ps_globals.mod DIRECTLY whenever session.save_handler is ini_set() at
 * runtime - completely bypassing this file, which has no way to intercept
 * that write (it lives in php-src). If a script switches away from "files"
 * and back to "files" again within one request, our patch from
 * req_apply() is gone with no trace, and the rest of that request's
 * session_start() calls silently run unprotected. We do not accept "no
 * trace in the logs" as a valid outcome (a leak with no log line is much
 * worse than a validated pass-through), so
 * fpm_coop_session_patch_req_enter() (called on EVERY fiber resume in the
 * request's life, not just its first entry - the coop model's own scheduler
 * checkpoint) re-runs the exact same check-and-patch logic req_apply() does.
 * In practice this makes the variant SELF-HEALING within one scheduler tick
 * of the ini_set() (the next time this fiber is resumed after any
 * suspension, or the next request if the fiber never suspends again before
 * finishing) rather than merely detecting-and-warning-but-staying-broken;
 * the first time this actually re-patches something (as opposed to finding
 * nothing to do), it logs once at WARNING per process so this is visible in
 * practice, not just in theory.
 */

#include "fpm_config.h"

#include "php.h"
#include "zend_ini.h"
#include "zend_hash.h"
#include "ext/session/php_session.h"

#include "fpm_pool_coop.h"
#include "fpm_pool_coop_session_patch.h"
#include "fpm_pool_fiber.h"
#include "zlog.h"

typedef struct psw_waiter_s {
	void *handle;
	struct psw_waiter_s *next;
} psw_waiter;

typedef struct psw_entry_s {
	bool held;
	struct fpm_coop_req_s *owner;
	psw_waiter *waiters_head;
	psw_waiter *waiters_tail;
} psw_entry;

typedef struct psw_mod_data_s {
	void *inner;
	zend_string *locked_key;
} psw_mod_data;

static bool psw_ready = false;
static const ps_module *psw_orig;
static void *psw_globals_addr;
static HashTable psw_lock_table;
static HashTable psw_owners;
static struct fpm_coop_req_s *psw_current_ctx;
static bool psw_rebind_warned = false;
static bool psw_nowait_warned = false;

static void psw_entry_val_dtor(zval *pDest) /* {{{ */
{
	psw_entry *e = (psw_entry *) Z_PTR_P(pDest);
	psw_waiter *w = e->waiters_head;

	while (w) {
		psw_waiter *next = w->next;
		efree(w);
		w = next;
	}
	efree(e);
}
/* }}} */

static void psw_waiter_enqueue(psw_entry *e, void *handle) /* {{{ */
{
	psw_waiter *w = emalloc(sizeof(*w));

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

/* Returns true if it actually woke someone - caller must NOT free/delete e
 * in that case, only the woken waiter's own eventual release may do so
 * (matches the use-after-free fix applied to the files_arb variant during
 * measurement - see docs/session-lock-arbiter-report.md, "Bug found and
 * fixed"). */
static bool psw_wake_one(psw_entry *e) /* {{{ */
{
	psw_waiter *w = e->waiters_head;

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

static void psw_set_owner(psw_entry *e, struct fpm_coop_req_s *ctx) /* {{{ */
{
	e->owner = ctx;
	if (ctx) {
		zend_hash_index_update_ptr(&psw_owners, (zend_ulong)(uintptr_t) ctx, e);
	}
}
/* }}} */

static void psw_clear_owner(psw_entry *e) /* {{{ */
{
	if (e->owner) {
		zend_hash_index_del(&psw_owners, (zend_ulong)(uintptr_t) e->owner);
		e->owner = NULL;
	}
}
/* }}} */

static bool psw_lock_acquire(zend_string *key) /* {{{ */
{
	psw_entry *e = zend_hash_find_ptr(&psw_lock_table, key);

	if (!e) {
		e = ecalloc(1, sizeof(*e));
		zend_hash_update_ptr(&psw_lock_table, key, e);
	}

	while (e->held) {
		void *waiter;

		if (!fpm_pool_fiber_can_wait()) {
			if (!psw_nowait_warned) {
				psw_nowait_warned = true;
				zlog(ZLOG_WARNING, "[pool %s] coop-session-patch: cannot suspend the current fiber to wait "
					"for an in-process session lock (nested user Fiber?) - proceeding without it "
					"(logged once per process)", fpm_coop_pool_name());
			}
			return false;
		}

		waiter = fpm_pool_fiber_waiter();
		psw_waiter_enqueue(e, waiter);
		fpm_pool_fiber_wait_wake(NULL);
	}

	e->held = true;
	psw_set_owner(e, psw_current_ctx);
	return true;
}
/* }}} */

static void psw_lock_release(zend_string *key) /* {{{ */
{
	psw_entry *e = zend_hash_find_ptr(&psw_lock_table, key);

	if (!e) {
		return;
	}

	e->held = false;
	psw_clear_owner(e);

	if (!psw_wake_one(e) && !e->waiters_head) {
		zend_hash_del(&psw_lock_table, key);
	}
}
/* }}} */

static zend_result psw_open(PS_OPEN_ARGS) /* {{{ */
{
	psw_mod_data *d = ecalloc(1, sizeof(*d));

	*mod_data = d;
	return psw_orig->s_open(&d->inner, save_path, session_name);
}
/* }}} */

static zend_result psw_close(PS_CLOSE_ARGS) /* {{{ */
{
	psw_mod_data *d = *mod_data;
	zend_result ret;

	if (!d) {
		return SUCCESS;
	}

	ret = psw_orig->s_close(&d->inner);

	if (d->locked_key) {
		psw_lock_release(d->locked_key);
		zend_string_release_ex(d->locked_key, false);
		d->locked_key = NULL;
	}

	efree(d);
	*mod_data = NULL;
	return ret;
}
/* }}} */

static zend_result psw_read(PS_READ_ARGS) /* {{{ */
{
	psw_mod_data *d = *mod_data;

	if (!d) {
		return FAILURE;
	}
	if (psw_lock_acquire(key) && !d->locked_key) {
		d->locked_key = zend_string_copy(key);
	}
	return psw_orig->s_read(&d->inner, key, val, maxlifetime);
}
/* }}} */

static zend_result psw_write(PS_WRITE_ARGS) /* {{{ */
{
	psw_mod_data *d = *mod_data;
	if (!d) return FAILURE;
	return psw_orig->s_write(&d->inner, key, val, maxlifetime);
}
/* }}} */

static zend_result psw_destroy(PS_DESTROY_ARGS) /* {{{ */
{
	psw_mod_data *d = *mod_data;
	if (!d) return FAILURE;
	return psw_orig->s_destroy(&d->inner, key);
}
/* }}} */

static zend_long psw_gc(PS_GC_ARGS) /* {{{ */
{
	psw_mod_data *d = *mod_data;
	if (!d) { *nrdels = 0; return 0; }
	return psw_orig->s_gc(&d->inner, maxlifetime, nrdels);
}
/* }}} */

static zend_string *psw_create_sid(PS_CREATE_SID_ARGS) /* {{{ */
{
	psw_mod_data *d = *mod_data;
	if (!d) return NULL;
	return psw_orig->s_create_sid(&d->inner);
}
/* }}} */

static zend_result psw_validate_sid(PS_VALIDATE_SID_ARGS) /* {{{ */
{
	psw_mod_data *d = *mod_data;
	if (!d) return FAILURE;
	return psw_orig->s_validate_sid(&d->inner, key);
}
/* }}} */

static zend_result psw_update_timestamp(PS_UPDATE_TIMESTAMP_ARGS) /* {{{ */
{
	psw_mod_data *d = *mod_data;
	if (!d) return FAILURE;
	return psw_orig->s_update_timestamp(&d->inner, key, val, maxlifetime);
}
/* }}} */

static ps_module psw_mod; /* s_name filled in at container-start from the
                            * captured original, so ini_get() output is
                            * unaffected by this patch. */

/* Reads ps_globals's address the same way fpm_pool_coop_session.c does
 * (duplicated here deliberately - see this file's header comment - so this
 * variant has no compile-time dependency on that file). Returns NULL if the
 * trick doesn't resolve or the selfcheck fails. */
static void *psw_resolve_globals_addr(void) /* {{{ */
{
	zend_ini_entry *entry = zend_hash_str_find_ptr(EG(ini_directives), ZEND_STRL("session.save_path"));
	php_ps_globals *ps;
	zend_long ini_val;

	if (!entry || !entry->mh_arg2) {
		return NULL;
	}
	ps = (php_ps_globals *) entry->mh_arg2;
	ini_val = zend_ini_long(ZEND_STRL("session.cookie_lifetime"), 0);
	if (ps->cookie_lifetime != ini_val) {
		return NULL;
	}
	return (void *) ps;
}
/* }}} */

void fpm_coop_session_patch_container_start(void) /* {{{ */
{
	php_ps_globals *ps;

	if (zend_get_module_started("session") != SUCCESS) {
		return;
	}

	psw_globals_addr = psw_resolve_globals_addr();
	if (!psw_globals_addr) {
		zlog(ZLOG_WARNING, "[pool %s] coop-session-patch: session.save_path ini-entry trick did not resolve "
			"- in-process session-lock patch variant NOT installed", fpm_coop_pool_name());
		return;
	}

	/* Called BEFORE this process's first php_request_startup(): RINIT has
	 * never run, so ps->mod can only hold whatever OnUpdateSaveHandler set
	 * at MINIT for the configured default - see header comment for why this
	 * timing IS the identification mechanism. */
	ps = (php_ps_globals *) psw_globals_addr;
	if (!ps->mod || !ps->mod->s_name || strcasecmp(ps->mod->s_name, "files") != 0) {
		zlog(ZLOG_NOTICE, "[pool %s] coop-session-patch: session.save_handler default is not \"files\" at "
			"container start (got %s) - patch variant stays inert for this pool for the life of the "
			"process (it does not retry later)", fpm_coop_pool_name(),
			(ps->mod && ps->mod->s_name) ? ps->mod->s_name : "(none)");
		return;
	}

	psw_orig = ps->mod;
	psw_mod = *psw_orig; /* copies s_name pointer too - ini_get() unaffected */
	psw_mod.s_open = psw_open;
	psw_mod.s_close = psw_close;
	psw_mod.s_read = psw_read;
	psw_mod.s_write = psw_write;
	psw_mod.s_destroy = psw_destroy;
	psw_mod.s_gc = psw_gc;
	psw_mod.s_create_sid = psw_create_sid;
	psw_mod.s_validate_sid = psw_validate_sid;
	psw_mod.s_update_timestamp = psw_update_timestamp;

	zend_hash_init(&psw_lock_table, 8, NULL, psw_entry_val_dtor, 0);
	zend_hash_init(&psw_owners, 8, NULL, NULL, 0);
	psw_ready = true;

	zlog(ZLOG_NOTICE, "[pool %s] coop-session-patch: in-process session-lock patch variant installed - "
		"will overwrite ps_globals.mod after each request's RINIT whenever session.save_handler resolves "
		"to the built-in \"files\" module; session.save_handler = files needs NO reconfiguration for this "
		"variant (compare to the files_arb variant, which does)", fpm_coop_pool_name());
}
/* }}} */

/* Shared by req_apply() (once, right after RINIT) and req_enter() (every
 * resume, self-healing against ini_set() mid-request - see header comment).
 * warn_on_rebind: log once if we find ourselves re-patching something that
 * was NOT already our wrapper (i.e. something switched it back to the real
 * "files" module since we last looked) - the ini_set() detection case. */
static void psw_check_and_apply(bool warn_on_rebind) /* {{{ */
{
	php_ps_globals *ps;
	const char *value;

	if (!psw_ready) {
		return;
	}

	ps = (php_ps_globals *) psw_globals_addr;
	if (ps->mod == &psw_mod) {
		return; /* already ours, nothing to do */
	}

	if (ps->session_status == php_session_active) {
		/* A session is CURRENTLY OPEN through a module that is not our
		 * wrapper - this is exactly the session.auto_start=1 case: RINIT
		 * ran the real, unwrapped "files" module's s_open()/s_read() before
		 * this hook ever got a chance to run, so ps->mod_data already holds
		 * a real ps_files* the real module allocated, not a psw_mod_data*.
		 * Swapping ps->mod out from under it here would leave ps->mod_data
		 * unchanged but ps->mod pointing at our wrapper - the next
		 * s_write()/s_close() (psw_write()/psw_close()) would then
		 * misinterpret that raw ps_files* as a psw_mod_data*, reading
		 * garbage out of unrelated struct offsets. Reproduced as a
		 * reliable SIGSEGV in ps_files_open() (data->last_key read as
		 * garbage) while measuring the auto_start gap - see
		 * docs/session-lock-arbiter-report.md, "Bug found and fixed during
		 * Variant 2 measurement". Do not swap while a session is active;
		 * the next check (this same function, called again from the next
		 * req_enter after session_write_close()/RSHUTDOWN closes it) picks
		 * this up safely once nothing is open to corrupt - which is
		 * exactly the "protects the second session_start() in the same
		 * request" property the design already claims, just enforced
		 * correctly instead of assumed. */
		return;
	}

	/* zend_ini_string_literal() is a newer Zend-core convenience macro not
	 * present in every php-src snapshot this project targets (confirmed
	 * absent from Zend/zend_ini.h on the build actually used for this
	 * measurement pass) - call the underlying zend_ini_string() directly,
	 * same effect, no macro dependency. */
	value = zend_ini_string(ZEND_STRL("session.save_handler"), 0);
	if (!value || strcasecmp(value, "files") != 0) {
		return; /* genuinely not "files" right now - stay inert */
	}

	if (warn_on_rebind && !psw_rebind_warned) {
		psw_rebind_warned = true;
		zlog(ZLOG_WARNING, "[pool %s] coop-session-patch: detected session.save_handler resolving back to "
			"the real \"files\" module mid-request (an ini_set() bypassed the patch) - re-applying the "
			"in-process lock wrapper now (logged once per process)", fpm_coop_pool_name());
	}

	ps->mod = &psw_mod;
}
/* }}} */

void fpm_coop_session_patch_req_apply(void) /* {{{ */
{
	psw_check_and_apply(false);
}
/* }}} */

void fpm_coop_session_patch_req_enter(struct fpm_coop_req_s *ctx) /* {{{ */
{
	if (!psw_ready) {
		return;
	}
	psw_current_ctx = ctx;
	psw_check_and_apply(true);
}
/* }}} */

void fpm_coop_session_patch_req_free(struct fpm_coop_req_s *ctx) /* {{{ */
{
	psw_entry *e;

	if (!psw_ready) {
		return;
	}

	e = zend_hash_index_find_ptr(&psw_owners, (zend_ulong)(uintptr_t) ctx);
	if (!e) {
		return;
	}

	zlog(ZLOG_WARNING, "[pool %s] coop-session-patch: request ctx #%u freed while still attributed as the "
		"holder of an in-process session lock - force-releasing it (leak safety net)",
		fpm_coop_pool_name(), ctx->id);

	e->held = false;
	psw_clear_owner(e);
	psw_wake_one(e);
}
/* }}} */
