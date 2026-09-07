/* fpm-ng: isolate INI entry VALUES (ini_set/ini_get) per request in the coop
 * executor (Fiber).
 *
 * Problem (measured on Symfony, endpoint returning ini_get('session.save_handler')
 * under 8 concurrent requests): fpm_pool_coop.c performs ONE
 * php_request_startup() for the container process, so EG(ini_directives) — the
 * zend_ini_entry table — is SHARED by all in-flight requests. One request's
 * ini_set() writes directly to ini_entry->value, which is the same for every
 * other request while all are suspended in the same process. Until now we
 * restored changed entries to the baseline only at the END of the request
 * (zend_ini_deactivate() in fpm_pool_coop.c) — too late: CONCURRENT requests
 * (another Fiber in flight in the same process) see the ini_set() of a request
 * that is still running.
 *
 * Solution: exactly the same pattern as SG/OG/symbol_table in fpm_pool_coop.c
 * and ps_globals in fpm_pool_coop_session.c — swap state at every processor
 * entry/leave. Here "state" is ONLY ini_entry->value (and its accompanying
 * modified/orig_value/modifiable fields) for entries that THIS request changed
 * — NOT the whole EG(ini_directives) (shared with everyone; copying the whole
 * table on every Fiber switch would be pointlessly expensive).
 *
 * --- Why EG(modified_ini_directives) is sufficient as the iteration list -----
 *
 * zend_alter_ini_entry_ex (Zend/zend_ini.c) adds every changed entry to
 * EG(modified_ini_directives) (name -> zend_ini_entry*) and sets modified=true
 * on the entry, orig_value=the value BEFORE that change — but ONLY on the FIRST
 * change (if (!modified) {...}); later ini_set() calls for the same key in the
 * same "modification round" do not touch orig_value. Since the coop model runs
 * AT MOST ONE Fiber at a time (cooperative switching, not threads), and WE make
 * every request leave fully restore changed entries to baseline and clear
 * modified, orig_value seen by zend_alter_ini_entry_ex at the NEXT modification
 * (of the same request after resuming, or of another request that gets the
 * processor) is always the real baseline value (pool config/php_admin_value),
 * never another in-flight value. In other words, in this model
 * EG(modified_ini_directives) is EXACTLY the list of "what THIS currently running
 * request changed since its last entry" — provided nobody leaves with
 * modified=true. This file is that condition.
 *
 * --- What we move and how (ownership transfer, without refcounts) ----------
 *
 * On leave (fpm_coop_ini_req_leave): for every entry in
 * EG(modified_ini_directives), stash ITS CURRENT value (this request's own
 * value, for example "5" after ini_set('precision', '5')) in ctx->ini_values
 * (name -> zend_string*, POINTER TRANSFER, without zend_string_copy/release —
 * like memcpy SG/OG in fpm_pool_coop.c), restore ini_entry->value =
 * ini_entry->orig_value (baseline), modified = false, orig_value = NULL,
 * modifiable = orig_modifiable. Move the entire EG(modified_ini_directives)
 * table as a block to ctx->ini_mods (a simple pointer swap — HashTable* holds
 * only zend_ini_entry* values that do not belong to us) and clear
 * EG(modified_ini_directives), so the next request in this process starts from
 * a clean "nothing changed" state.
 *
 * On entry (fpm_coop_ini_req_enter): for every entry in ctx->ini_mods, restore
 * exactly what zend_alter_ini_entry_ex itself would do: the current (baseline)
 * entry value becomes orig_value, this request's value from ctx->ini_values
 * returns to ini_entry->value, modified = true. Return ctx->ini_mods as
 * EG(modified_ini_directives) (again a pointer swap) — so subsequent ini_set()
 * IN THIS request and final zend_ini_deactivate() at request end
 * (fpm_pool_coop.c) behave exactly as if nothing had happened.
 *
 * We DELIBERATELY do NOT call ini_entry->on_modify() on any switch — see the
 * section "What this isolation does NOT fix" below.
 *
 * --- Cheap path -------------------------------------------------------------
 *
 * A request that did not touch INI since its last entry has
 * EG(modified_ini_directives) == NULL — exactly as zend_ini_deactivate() does
 * today. fpm_coop_ini_req_leave() checks this FIRST and returns without an
 * allocation or a walk over any table. fpm_coop_ini_req_enter() checks
 * ctx->ini_mods == NULL just as cheaply. This is the path for EVERY Fiber
 * switch that does not use ini_set/set_time_limit/session_set_save_handler/...
 * — that is, the overwhelming majority of switches.
 *
 * --- What this isolation does NOT fix (stated explicitly) -------------------
 *
 * Many on_modify callbacks (Zend/zend_ini.c: OnUpdateLong/OnUpdateBool/...) do
 * more than set ini_entry->value — they also copy the parsed value to a
 * PROCESS-WIDE field through ZEND_INI_GET_ADDR (mh_arg1 = offset, mh_arg2 = the
 * module-global base address; in an NTS build module globals are ONE instance
 * for the whole process, like core_globals). This file swaps ONLY
 * ini_entry->value/orig_value/modified — it does NOT call on_modify on a
 * switch (see the rationale above: safety and cost), so THAT PROCESS-WIDE FIELD
 * is not switched. Example: ini_set('precision', '5') fixes ini_get('precision')
 * ONLY for this request (fixed here), but the real precision used by
 * var_dump/serialize (core_globals.precision, set by OnUpdateLong) remains
 * process-wide while it is live — so it may leak to other in-flight requests
 * despite this fix. The only exception in this codebase is ext/session:
 * PS(mod) (set by OnUpdateSaveHandler) is safe because all ps_globals (including
 * PS(mod)) are ALREADY swapped separately, per request, by
 * fpm_pool_coop_session.c — independently of this file. Fixing the general case
 * would require swapping the globals of EVERY module with on_modify (like
 * ps_globals) — that is not what this report covers (the measured symptom is
 * specifically that ini_get() sees another value), so we stop here and say so
 * explicitly instead of pretending to provide full isolation.
 */

#include "fpm_config.h"

#include "php.h"
#include "zend_ini.h"

#include "fpm_pool_coop.h"
#include "fpm_pool_coop_ini.h"

void fpm_coop_ini_req_leave(struct fpm_coop_req_s *ctx) /* {{{ */
{
	zend_ini_entry *entry;
	zend_string *name;
	HashTable *values;

	if (!EG(modified_ini_directives)) {
		return;
	}

	ALLOC_HASHTABLE(values);
	zend_hash_init(values, zend_hash_num_elements(EG(modified_ini_directives)), NULL, NULL, false);

	ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(EG(modified_ini_directives), name, entry) {
		/* This request's OWN value — stash it by transferring the pointer. */
		zend_hash_add_ptr(values, name, entry->value);

		/* Restore the baseline to the entry, exactly as zend_restore_ini_entry_cb
		 * does — except for calling on_modify; see the rationale at the top. */
		entry->value = entry->orig_value;
		entry->modifiable = entry->orig_modifiable;
		entry->modified = false;
		entry->orig_value = NULL;
		entry->orig_modifiable = false;
	} ZEND_HASH_FOREACH_END();

	ctx->ini_values = values;
	ctx->ini_mods = EG(modified_ini_directives);
	EG(modified_ini_directives) = NULL;
}
/* }}} */

void fpm_coop_ini_req_enter(struct fpm_coop_req_s *ctx) /* {{{ */
{
	zend_ini_entry *entry;
	zend_string *name;
	zend_string *value;

	if (!ctx->ini_mods) {
		return;
	}

	ZEND_HASH_MAP_FOREACH_STR_KEY_PTR(ctx->ini_mods, name, entry) {
		value = zend_hash_find_ptr(ctx->ini_values, name);

		/* Exactly what zend_alter_ini_entry_ex does on the first modification
		 * in a "round": current (baseline) entry value -> orig_value, this
		 * request's value -> value. Transfer the pointer, without
		 * zend_string_copy/release. */
		entry->orig_value = entry->value;
		entry->orig_modifiable = entry->modifiable;
		entry->value = value;
		entry->modified = true;
	} ZEND_HASH_FOREACH_END();

	/* ini_values are no longer needed — values were transferred above. Its
	 * NULL destructor means destroy does not release anybody's strings. */
	zend_hash_destroy(ctx->ini_values);
	FREE_HASHTABLE(ctx->ini_values);
	ctx->ini_values = NULL;

	/* Return the table as EG(modified_ini_directives) — subsequent ini_set() in
	 * this request and final zend_ini_deactivate() at request end
	 * (fpm_pool_coop.c) work as if the request had never left the processor. */
	EG(modified_ini_directives) = ctx->ini_mods;
	ctx->ini_mods = NULL;
}
/* }}} */

void fpm_coop_ini_req_free(struct fpm_coop_req_s *ctx) /* {{{ */
{
	zend_string *value;

	/* The normal path does not enter here: the request ends ON THE PROCESSOR,
	 * so the last fpm_coop_ini_req_enter() has returned both tables, and
	 * zend_ini_deactivate() at request end restored entries through on_modify.
	 * This protects against destroying a request context that left the processor
	 * and never returned (for example, a Fiber killed while the worker shuts
	 * down). The INI entries are already at baseline — fpm_coop_ini_req_leave()
	 * restored them before leaving — so only this request's orphaned values remain
	 * to be released. */
	if (ctx->ini_values) {
		ZEND_HASH_MAP_FOREACH_PTR(ctx->ini_values, value) {
			zend_string_release(value);
		} ZEND_HASH_FOREACH_END();
		zend_hash_destroy(ctx->ini_values);
		FREE_HASHTABLE(ctx->ini_values);
		ctx->ini_values = NULL;
	}
	if (ctx->ini_mods) {
		zend_hash_destroy(ctx->ini_mods);
		FREE_HASHTABLE(ctx->ini_mods);
		ctx->ini_mods = NULL;
	}
}
/* }}} */
