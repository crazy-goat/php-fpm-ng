/* fpm-ng: per-request isolation of a CONFIGURED list of class static
 * properties on the coop (fiber) executor.
 *
 * Problem (tasks/008-laravel-class-statics.md, measured on Laravel 13.30.1,
 * pool.executor = fiber, FPMNG_SHARED_INCLUDES=1, pm.max_children = 1,
 * 8 concurrent requests to /session): class static properties are, like
 * everything the coop model shares by default (see fpm_pool_coop.h), one
 * copy per PROCESS. Laravel keeps its whole request state reachable from
 * two of them -- Illuminate\Container\Container::$instance and
 * Illuminate\Support\Facades\Facade::$app/$resolvedInstance. Request B
 * bootstraps its own Application and overwrites them; when fiber A resumes
 * from I/O, app()/session()/DB::/Redis:: all resolve inside B's container.
 * Measured: 5 of 8 concurrent requests silently returned another user's
 * session and another user's Application/Request objects (same object
 * identity), with 8x200 and an empty log -- worse than the pre-isolation
 * failure mode (1x200, 4x500, 3 timeouts), because it looks like success.
 *
 * Fix: dokladnie ten sam wzorzec co SG/OG/symbol_table (fpm_pool_coop.c),
 * ps_globals (fpm_pool_coop_session.c) and ini_entry->value
 * (fpm_pool_coop_ini.c) -- swap state at enter/leave. Unlike those three,
 * "state" here is not a whole struct or a whole module's globals: it is a
 * handful of individual zval slots, named by pool configuration
 * (fiber.isolate_statics), not by C. Laravel's class names belong in
 * configuration and documentation (docs/frameworks.md) -- there is nothing
 * Laravel-specific below.
 *
 * Directive: fiber.isolate_statics = Class\Name::property[,Other::property]
 * (declaring class, PHP property name without the leading '$'). Comma-
 * separated, same convention as http.allowed_clients/http.trusted_proxies.
 * Rejected outright for every pool type that isn't a fiber-executor one --
 * for free, via the existing rejects[] mechanism: "fiber." is already a
 * rejected prefix everywhere except the two coop pool-type variants (see
 * fpm_pool_fastcgi_rejects &c. and fpm_coop_rejects in fpm_pool_type.c/
 * fpm_pool_coop.c). Nothing in this file compares a pool-type name.
 *
 * --- Resolving "Class::property" to a live zval slot ------------------
 *
 * ce = zend_hash_str_find_ptr(EG(class_table), <lowercased class name>) --
 * class_table is keyed lowercase. NOT zend_lookup_class(): that triggers the
 * autoloader, and this code runs from inside fpm_coop_req_enter/leave, i.e.
 * at points the request script did not choose -- invoking user autoload
 * code there would be a surprise side effect. A class not yet loaded (not
 * autoloaded yet, or this request's script never touches it) is handled the
 * same as "does not exist": ce is NULL, we skip this item for this
 * enter/leave and try again next time. This is the answer to the lazy-
 * loading hazard for the CLASS half of the question.
 *
 * info = zend_hash_find_ptr(&ce->properties_info, <property name>) --
 * ce->properties_info is populated for BOTH declared and inherited
 * properties. Crucially, for a static property that a subclass inherits
 * WITHOUT redeclaring, do_inherit_property() (Zend/zend_inheritance.c) does
 *     _zend_hash_append_ptr(&ce->properties_info, key, parent_info)
 * -- the *same* zend_property_info* as the declaring class, not a copy.
 * info->ce is therefore always the class that actually DECLARES the
 * property, regardless of which class name (subclass or the declaring
 * class itself) the admin named in configuration. We resolve the storage
 * slot through info->ce and info->offset, never through the configured
 * class -- so configuring a subclass name never yields a *different*
 * physical slot than configuring the declaring class name would. This is
 * the fix for the "inherited statics" hazard: the spike resolved through
 * CE_STATIC_MEMBERS(<configured class>) and relied on ZVAL_DEINDIRECT to
 * land on the right slot, which is correct on its own (see
 * zend_std_get_static_property_with_info() in Zend/zend_object_handlers.c,
 * which does exactly that), but leaves a second failure mode unaddressed:
 * two configured entries that are the *same* declaring slot under two
 * names (e.g. both "Base::x" and "Sub::x" configured by mistake) would be
 * swapped twice within one enter/leave pass, corrupting state. Resolving
 * via info->ce collapses both configured names onto one canonical address,
 * so the duplicate-detection pass below (over resolved addresses, not over
 * configured names) actually catches it.
 *
 * slot = CE_STATIC_MEMBERS(info->ce) + info->offset, then ZVAL_DEINDIRECT
 * (cheap, and info->ce is by construction never itself the indirect
 * pointer -- it is the declaring class -- so this is a defensive no-op in
 * the ordinary case, kept only because zend_std_get_static_property_with_info
 * always does it too and there is no reason to be less careful than the
 * engine's own accessor).
 *
 * CE_STATIC_MEMBERS(info->ce) can be NULL: the runtime statics table is
 * allocated lazily, by zend_class_init_statics() (Zend/zend_object_
 * handlers.c), on first access to ANY static of that class (in that class
 * or, per the child-inherits-without-redeclaring case above, in a
 * subclass). A configured class whose statics have never been touched on
 * this process is not an error -- there is nothing to isolate yet -- so we
 * skip. This is the answer to the lazy-init hazard for the PROPERTY half:
 * both "class not loaded" and "class loaded but statics not initialised"
 * degrade to the same "skip this item, try again next enter/leave" path,
 * with no error and no crash. We deliberately do NOT call
 * zend_class_init_statics()/zend_update_class_constants() ourselves: doing
 * so from inside enter/leave, on a class the request may never actually
 * touch, would force initialisation (and constant-expression evaluation)
 * at a point the script did not choose, which is exactly the kind of
 * surprise side effect this file avoids for the autoloader case too.
 *
 * A second, easy-to-miss lazy-init case: CE_STATIC_MEMBERS(info->ce) being
 * non-NULL does not mean info->ce->ce_flags has ZEND_ACC_CONSTANTS_UPDATED
 * -- default_static_members_table entries can still be an unresolved
 * IS_CONSTANT_AST if constants have not been updated yet, and this file
 * reads that same table (see "restoring the default" below), so it checks
 * ZEND_ACC_CONSTANTS_UPDATED explicitly and skips the item if it is not
 * set, rather than assume the ordering zend_std_get_static_property_with_info
 * happens to use (constants-update, then init-statics).
 *

 * --- Property-offset stability across zend_update_class_constants ------
 *
 * info->offset is assigned once, when the class is linked (either at
 * declaration, in zend_declare_typed_property(), or at inheritance time, in
 * zend_do_inherit_property()/zend_do_inherit_properties() in
 * Zend/zend_inheritance.c, both of which run long before any request
 * executes). We never cache ce, info, or an offset across requests or
 * across enter/leave calls -- every call re-resolves both class and
 * property by name through the live hash tables at that exact moment.
 * There is therefore nothing in this file that COULD be invalidated by a
 * later zend_update_class_constants(): if the offset had somehow changed
 * between two enter/leave calls (it doesn't, once a class is fully linked
 * and living in EG(class_table) -- the coop model runs the whole process
 * as one php_request_startup(), so a class, once loaded, stays loaded and
 * linked for the rest of the process, see fpm_pool_coop.h), the very next
 * lookup would already see the new value, not a stale cached one.
 *
 * --- Cached slot pointers (runtime cache / CACHE_SLOT) ------------------
 *
 * ZEND_FETCH_STATIC_PROP_* opcodes cache a pointer to the *slot* (the zval
 * inside CE_STATIC_MEMBERS(ce)), not a copy of its contents. This file
 * mutates the slot's CONTENTS in place (ZVAL_COPY_VALUE/ZVAL_UNDEF) and
 * never reallocates or moves CE_STATIC_MEMBERS(ce) itself, so any pointer
 * a cache slot holds into that array stays valid across our swap -- the
 * cache is not stale, it is pointing at whatever we last put there, which
 * is precisely what this request should see. Mitigating factor, checked
 * directly in this codebase rather than assumed from the task description:
 * pool.executor = fiber still rejects opcache.enable = 1 at pool-validate
 * time (fpm_coop_validate(), fpm_pool_coop.c, fpm_coop_opcache_msg) as of
 * this commit, which narrows the exposure of any cache-related surprise
 * this reasoning has not accounted for -- opcache's own SHM-backed
 * ZEND_MAP_PTR indirection for statics is simply not in play here.
 *
 * --- Memory ownership: PROVEN, not assumed --------------------------------
 *
 * Taking a request's own value OUT of the live slot is
 * ZVAL_COPY_VALUE(dst, src) -- no incref/decref, exactly the existing
 * pattern for SG/OG/http_globals (fpm_pool_coop.c) and ini_entry->value/
 * orig_value (fpm_pool_coop_ini.c). The claim to prove is: "at the moment
 * of a context switch, the static property slot's zval is the only zval
 * that needs its OWN CONTENTS moved, and moving it (not copying it) cannot
 * corrupt any refcount, anywhere." Unlike SG/OG/ini_entry->value, the slot
 * is not then left as IS_UNDEF -- it is refilled with the class's own
 * compiled-in default (ZVAL_COPY_OR_DUP from
 * ce->default_static_members_table; see fpm_coop_statics_req_leave()) so
 * that any OTHER, unrelated request that touches this same property for
 * the first time while this one is away sees the class's real default, not
 * an artificial hole. That refill is ordinary, engine-sanctioned zval
 * duplication -- the same macro zend_class_init_statics() itself uses --
 * not a second "move", so it does not affect the argument below, which is
 * about the request's own value.
 *
 * ZVAL_COPY_VALUE copies the zval's bytes (type + value union -- for a
 * refcounted type, that union IS a pointer to a zend_refcounted*, e.g. an
 * object handle, an array pointer, or a zend_reference*) verbatim to a new
 * address. ZVAL_UNDEF(src) then sets ONLY the source zval's type tag to
 * IS_UNDEF; it does not touch the destination, and it does not touch
 * whatever zend_refcounted* the source used to point at. No refcount
 * anywhere is incremented or decremented by either step. The total number
 * of zval STORAGE LOCATIONS that hold a live pointer to that
 * zend_refcounted* is therefore unchanged by the operation: it held exactly
 * one such pointer (in the static slot) before, and it holds exactly one
 * (now in ctx->items[i].saved, or back in the slot) after. This is not "the
 * value happens to live in one place and we hope nothing else refers to
 * it" -- it is "we relocate the ONE zval that is the static property slot's
 * own storage; whatever refcount the object/array/reference it points to
 * has, from ALL of its other referrers combined, is completely unaffected,
 * because none of THOSE referrers is the thing we moved". This holds
 * regardless of how many OTHER zvals elsewhere (local variables, other
 * properties, closures) independently hold their own pointer to the same
 * zend_refcounted* with their own share of its refcount -- we never touch
 * their storage, only the static slot's.
 *
 * This also answers the GC-roots question directly: PHP's cycle collector
 * (Zend/zend_gc.c) tracks POSSIBLE ROOTS by zend_refcounted* (the object/
 * array header), via GC_ADDREF/gc_possible_root, never by the address of
 * the zval slot that happens to hold a pointer to it. Relocating the zval
 * that CONTAINS the pointer does not touch the header the collector tracks,
 * so it cannot desynchronise GC bookkeeping.
 *
 * References -- the case the spike explicitly left untested -- follow from
 * the same argument, because a zend_reference is itself just a refcounted
 * structure and the static slot's zval just holds a pointer to it
 * (Z_TYPE_P(slot) == IS_REFERENCE, Z_REF_P(slot) == that pointer) once
 * `&Class::$static` has run. `$x = &Class::$static;` increments the
 * zend_reference's own refcount by one, for $x's copy of the pointer; our
 * swap relocates the STATIC SLOT'S copy of the pointer (moves it into
 * ctx->items[i].saved and back), never touching $x's copy, and never
 * touching the zend_reference's refcount either way. $x keeps seeing
 * whatever the (correctly restored, at the next enter) static slot's
 * reference sees, exactly as PHP reference semantics require, across any
 * number of suspend/resume cycles. See tests/statics_reference.php,
 * committed alongside this file: it takes a reference to an isolated
 * static property, suspends the request (Fiber::suspend()) across it, and
 * asserts both directions of the reference still see each other's writes
 * after resuming, with the item correctly isolated from a second,
 * concurrently-run "other user" request in between.
 *
 * --- Duplicate slots, syntax errors, misconfiguration -------------------
 *
 * Syntax that cannot possibly be "Class::property" (no "::", an empty
 * class or property name) is rejected at pool-validate time --
 * fpm_coop_statics_validate(), called from fpm_pool_type_fiber_validate()
 * (fpm_pool_type.c) on the master side, before any child forks -- exactly
 * like a malformed http.allowed_clients/http.trusted_proxies ACL already
 * fails pool.type = http via fpm_http_acl_parse(). The pool does not start;
 * the admin sees the message immediately, same channel as any other
 * config error.
 *
 * A class that does not exist, or a property that is not static, CANNOT be
 * told apart from "not loaded yet" at validate time -- the autoloader has
 * not run, there is no script, there is nothing to inspect. These are
 * therefore a RUNTIME condition, logged once per item (not once per
 * request -- see item->warned) as a WARNING, with the pool continuing to
 * run and isolation silently skipped for that one item. This mirrors how
 * fpm_pool_coop_session.c treats its own equivalent case (missing ini
 * hook): warn and disable the specific path, never crash the worker.
 *
 * Two configured entries resolving to the same physical slot (see the
 * inherited-statics discussion above) are caught per enter/leave pass by
 * comparing resolved slot addresses, not configured names; the second one
 * is skipped and warned about once, rather than double-swapped.
 */

#include "fpm_config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "php.h"
#include "zend_API.h"
#include "zend_object_handlers.h"

#include "fpm_worker_pool.h"
#include "fpm_pool_coop.h"
#include "fpm_pool_coop_statics.h"
#include "fpm_pool_type.h"
#include "zlog.h"

/* Generous but bounded: this is admin-authored configuration, not request
 * input, so the cap only exists to keep the per-ctx array a fixed size and
 * to give a clear error instead of silently truncating a typo'd list. */
#define FPM_COOP_STATICS_MAX 64

struct fpm_coop_static_item {
	char *class_lower;
	size_t class_lower_len;
	char *prop_name;
	size_t prop_len;
	char label[192];	/* "Class::property" as configured, for logs */
	bool warned;		/* logged the runtime "not found" warning once already */
};

static struct fpm_coop_static_item fpm_coop_statics_items[FPM_COOP_STATICS_MAX];
static int fpm_coop_statics_count = 0;

static char *fpm_coop_statics_strdup_lower(const char *s, size_t len) /* {{{ */
{
	char *out = malloc(len + 1);
	size_t i;

	for (i = 0; i < len; i++) {
		out[i] = (char) tolower((unsigned char) s[i]);
	}
	out[len] = '\0';
	return out;
}
/* }}} */

/* Splits "Class::prop,Other::prop2" into (class, prop) pairs, one call per
 * entry via cb -- shared by the master-side syntax check (validate, no
 * side effects beyond cb's own bookkeeping) and the child-side item build
 * (container_start). Returns 0, or -1 on a syntax error (message already
 * logged into *errbuf). */
typedef void (*fpm_coop_statics_cb)(const char *class_name, size_t class_len,
	const char *prop_name, size_t prop_len, void *ctx);

static int fpm_coop_statics_foreach(const char *raw, fpm_coop_statics_cb cb, void *cb_ctx,
	char *errbuf, size_t errbuf_len) /* {{{ */
{
	char *copy, *save, *tok;
	int n = 0;

	if (!raw || !*raw) {
		return 0;
	}

	copy = strdup(raw);
	for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *sep;
		size_t class_len, prop_len;

		while (*tok == ' ' || *tok == '\t') {
			tok++;
		}
		sep = strstr(tok, "::");
		if (!sep) {
			snprintf(errbuf, errbuf_len, "entry '%s' has no '::' -- expected Class\\Name::property", tok);
			free(copy);
			return -1;
		}
		class_len = (size_t) (sep - tok);
		prop_len = strlen(sep + 2);
		if (class_len == 0 || prop_len == 0) {
			snprintf(errbuf, errbuf_len, "entry '%s' is missing a class or property name", tok);
			free(copy);
			return -1;
		}
		if (n >= FPM_COOP_STATICS_MAX) {
			snprintf(errbuf, errbuf_len, "more than %d entries in fiber.isolate_statics", FPM_COOP_STATICS_MAX);
			free(copy);
			return -1;
		}
		cb(tok, class_len, sep + 2, prop_len, cb_ctx);
		n++;
	}
	free(copy);
	return 0;
}
/* }}} */

static void fpm_coop_statics_validate_cb(const char *class_name, size_t class_len,
	const char *prop_name, size_t prop_len, void *cb_ctx) /* {{{ */
{
	(void) class_name; (void) class_len; (void) prop_name; (void) prop_len; (void) cb_ctx;
	/* Syntax already checked by the caller; nothing else to do here at
	 * validate time -- see the file header for why class/property
	 * existence cannot be checked yet. */
}
/* }}} */

int fpm_coop_statics_validate(struct fpm_worker_pool_s *wp) /* {{{ */
{
	char errbuf[256];

	if (!wp->config->fiber_isolate_statics || !*wp->config->fiber_isolate_statics) {
		return 0;
	}
	if (fpm_coop_statics_foreach(wp->config->fiber_isolate_statics, fpm_coop_statics_validate_cb, NULL,
			errbuf, sizeof(errbuf)) != 0) {
		zlog(ZLOG_ALERT, "[pool %s] fiber.isolate_statics: %s", wp->config->name, errbuf);
		return -1;
	}
	return 0;
}
/* }}} */

static void fpm_coop_statics_build_cb(const char *class_name, size_t class_len,
	const char *prop_name, size_t prop_len, void *cb_ctx) /* {{{ */
{
	const char *pool_name = (const char *) cb_ctx;
	struct fpm_coop_static_item *item = &fpm_coop_statics_items[fpm_coop_statics_count];

	item->class_lower = fpm_coop_statics_strdup_lower(class_name, class_len);
	item->class_lower_len = class_len;
	item->prop_name = malloc(prop_len + 1);
	memcpy(item->prop_name, prop_name, prop_len);
	item->prop_name[prop_len] = '\0';
	item->prop_len = prop_len;
	item->warned = false;
	snprintf(item->label, sizeof(item->label), "%.*s::%s", (int) class_len, class_name, item->prop_name);
	fpm_coop_statics_count++;

	zlog(ZLOG_NOTICE, "[pool %s] coop-statics: [%d] %s", pool_name, fpm_coop_statics_count - 1, item->label);
}
/* }}} */

void fpm_coop_statics_container_start(const char *pool_name) /* {{{ */
{
	struct fpm_worker_pool_s *wp = fpm_pool_type_current_pool();
	char errbuf[256];

	fpm_coop_statics_count = 0;

	if (!wp || !wp->config->fiber_isolate_statics || !*wp->config->fiber_isolate_statics) {
		/* Empty (the default) or, defensively, no current pool found --
		 * the whole mechanism is off, at zero cost: every hook below is
		 * a single "fpm_coop_statics_count == 0" check. */
		return;
	}

	if (fpm_coop_statics_foreach(wp->config->fiber_isolate_statics, fpm_coop_statics_build_cb,
			(void *) pool_name, errbuf, sizeof(errbuf)) != 0) {
		/* Cannot happen: fpm_coop_statics_validate() already rejected this
		 * exact string on the master side. If it ever does (a future
		 * change desyncs the two checks), fail safe -- no isolation,
		 * loudly -- rather than run with a half-built item list. */
		zlog(ZLOG_ALERT, "[pool %s] coop-statics: '%s' passed validate() but not container_start() (%s) -- "
			"isolation of class statics DISABLED for this pool", pool_name,
			wp->config->fiber_isolate_statics, errbuf);
		fpm_coop_statics_count = 0;
		return;
	}

	zlog(ZLOG_NOTICE, "[pool %s] coop-statics: per-request isolation of %d class static propert%s ENABLED",
		pool_name, fpm_coop_statics_count, fpm_coop_statics_count == 1 ? "y" : "ies");
}
/* }}} */

/* Resolves one configured item to its live slot plus the class's compiled-in
 * DEFAULT for that same property (ce->default_static_members_table), or
 * returns false when there is nothing to isolate yet (class not loaded, or
 * loaded but its statics table has not been allocated, or its constant
 * expressions not yet resolved -- see file header, lazy-init hazard). Never
 * triggers autoload, never forces static initialisation.
 *
 * The default is needed by fpm_coop_statics_req_leave(): see the comment
 * there for why leaving the live slot as IS_UNDEF (the spike's approach,
 * and this file's own first version) is a genuine bug, not merely a style
 * choice, whenever the property has a declared type. */
static bool fpm_coop_statics_resolve(struct fpm_coop_static_item *item, zval **slot_out, zval **default_out) /* {{{ */
{
	zend_class_entry *ce;
	zend_property_info *info;

	ce = zend_hash_str_find_ptr(EG(class_table), item->class_lower, item->class_lower_len);
	if (!ce) {
		return false;
	}
	info = zend_hash_str_find_ptr(&ce->properties_info, item->prop_name, item->prop_len);
	if (!info || !(info->flags & ZEND_ACC_STATIC)) {
		if (!item->warned) {
			zlog(ZLOG_WARNING, "[pool %s] coop-statics: %s -- class loaded but no such static property; "
				"isolation skipped for this item, request runs unisolated for it", fpm_coop_pool_name(), item->label);
			item->warned = true;
		}
		return false;
	}
	/* Resolve through the DECLARING class (info->ce), never through the
	 * configured one -- see file header, inherited-statics discussion. */
	if (!(info->ce->ce_flags & ZEND_ACC_CONSTANTS_UPDATED)) {
		return false;	/* default_static_members_table may still hold an unresolved IS_CONSTANT_AST */
	}
	if (CE_STATIC_MEMBERS(info->ce) == NULL) {
		return false;	/* not yet initialised on this process; not an error */
	}
	*slot_out = CE_STATIC_MEMBERS(info->ce) + info->offset;
	ZVAL_DEINDIRECT(*slot_out);
	*default_out = &info->ce->default_static_members_table[info->offset];
	return true;
}
/* }}} */

void fpm_coop_statics_req_leave(struct fpm_coop_req_s *ctx) /* {{{ */
{
	int i, j;
	zval *slots;
	void *resolved[FPM_COOP_STATICS_MAX];

	if (fpm_coop_statics_count == 0) {
		return;
	}
	if (!ctx->statics) {
		ctx->statics = ecalloc(fpm_coop_statics_count, sizeof(zval)); /* IS_UNDEF everywhere (zero-fill) */
	}
	slots = (zval *) ctx->statics;

	for (i = 0; i < fpm_coop_statics_count; i++) {
		zval *slot, *def;

		if (!fpm_coop_statics_resolve(&fpm_coop_statics_items[i], &slot, &def)) {
			resolved[i] = NULL;
			continue;
		}
		for (j = 0; j < i; j++) {
			if (resolved[j] == (void *) slot) {
				/* Same declaring slot already moved by an earlier entry in
				 * this very pass -- two configured names collided on one
				 * physical property. Leave it where the first entry put
				 * it; do not move it again. */
				if (!fpm_coop_statics_items[i].warned) {
					zlog(ZLOG_WARNING, "[pool %s] coop-statics: %s resolves to the same property as %s -- "
						"configured twice under different names, isolating it once",
						fpm_coop_pool_name(), fpm_coop_statics_items[i].label, fpm_coop_statics_items[j].label);
					fpm_coop_statics_items[i].warned = true;
				}
				slot = NULL;
				break;
			}
		}
		resolved[i] = slot;
		if (!slot) {
			continue;
		}
		/* Move this request's own value out (not a copy -- see file header,
		 * memory-ownership section), THEN put the class's compiled-in
		 * DEFAULT back in the live slot -- never IS_UNDEF. A request that
		 * has never touched this item before (ctx->statics freshly
		 * allocated above, entry still IS_UNDEF) does nothing on its next
		 * enter() (see below), so between here and then the live slot is
		 * what any OTHER, unrelated request needs to see: the class's own
		 * fresh default, exactly as if nobody had ever suspended holding
		 * it away. Leaving IS_UNDEF instead (the spike's approach) is a
		 * real bug for any TYPED property with no nullable/default: PHP
		 * throws "must not be accessed before initialization" on the very
		 * first read by that other request -- reproduced with
		 * tests/statics_reference.php before this fix, see its git log
		 * entry. ZVAL_COPY_OR_DUP, not ZVAL_COPY_VALUE, because the
		 * default template can itself be a refcounted value (e.g. an
		 * array-literal default) that must not be aliased between the
		 * class's permanent default table and the live slot. */
		ZVAL_COPY_VALUE(&slots[i], slot);
		ZVAL_COPY_OR_DUP(slot, def);
	}
}
/* }}} */

void fpm_coop_statics_req_enter(struct fpm_coop_req_s *ctx) /* {{{ */
{
	int i;
	zval *slots;

	if (fpm_coop_statics_count == 0) {
		return;
	}
	if (!ctx->statics) {
		return; /* first time this ctx runs -- nothing was ever stashed for it */
	}
	slots = (zval *) ctx->statics;

	for (i = 0; i < fpm_coop_statics_count; i++) {
		zval *saved = &slots[i];
		zval *slot, *def;

		if (Z_TYPE_P(saved) == IS_UNDEF) {
			continue; /* nothing stashed for this item at the last leave() */
		}
		if (!fpm_coop_statics_resolve(&fpm_coop_statics_items[i], &slot, &def)) {
			/* The class "un-loaded" between leave() and enter() -- cannot
			 * happen (EG(class_table) only grows for the life of the
			 * process), but if it ever did, there is nowhere to give the
			 * value back to: keep it in ctx (the next enter() tries
			 * again) rather than segfault or silently drop a reference. */
			continue;
		}
		/* Whatever is currently in the live slot is the default placeholder
		 * this same req_leave() put there (or another request's leave(),
		 * if that request never got a turn in between) -- release it
		 * before overwriting, since ZVAL_COPY_OR_DUP may have allocated it
		 * (e.g. duplicated an array default). Then move this request's own
		 * value back in -- again a move, not a copy. */
		zval_ptr_dtor(slot);
		ZVAL_COPY_VALUE(slot, saved);
		ZVAL_UNDEF(saved);
	}
}
/* }}} */

void fpm_coop_statics_req_free(struct fpm_coop_req_s *ctx) /* {{{ */
{
	if (ctx->statics) {
		zval *slots = (zval *) ctx->statics;
		int i;

		/* Normal shutdown never reaches this with anything left to free:
		 * fpm_pool_coop.c sets ctx->live = false before the final leave(),
		 * and the final enter() before that already gave every stashed
		 * value back. This is a safety net for a ctx destroyed while
		 * suspended (e.g. a fiber killed at worker shutdown) -- release
		 * whatever it is still holding instead of leaking it. */
		for (i = 0; i < fpm_coop_statics_count; i++) {
			if (Z_TYPE(slots[i]) != IS_UNDEF) {
				zval_ptr_dtor(&slots[i]);
			}
		}
		efree(ctx->statics);
		ctx->statics = NULL;
	}
}
/* }}} */
