/* fpm-ng: isolate ext/session state per request in the coop executor (Fiber).
 *
 * Problem (docs/frameworks.md, "Symfony — PHP sessions"): ext/session keeps all
 * its state in the PROCESS — PS(session_status), PS(in_save_handler), $_SESSION
 * (PS(http_session_vars)), an open save handler (for example, a Redis
 * connection) — while the coop executor performs ONE php_request_startup() for
 * the process lifetime (fpm_pool_coop.c), so the session module's RINIT/RSHUTDOWN
 * NEVER run per request. Measured: Fiber A waits in Redis I/O inside the save
 * handler's read(), Fiber B calls session_start() in the same process and gets
 * "Cannot call session save handler in a recursive manner" — 20/20 runs, HTTP
 * 500. Sequentially (one request in flight) it works.
 *
 * Solution: exactly the same pattern as SG/OG/symbol_table in fpm_pool_coop.c —
 * swap module globals at enter/leave — plus call the session module's
 * RINIT/RSHUTDOWN PER REQUEST (the swap isolates data, while RINIT/RSHUTDOWN
 * provide the lifecycle: the session starts and is FLUSHED to storage for EVERY
 * request, not once per process).
 *
 * --- Address of ps_globals without a linker dependency ----------------------
 *
 * A hard `extern ps_globals` (ZEND_EXTERN_MODULE_GLOBALS(ps) by name in our
 * code) would create a linker dependency on a symbol that, with
 * --enable-session=shared, lives in session.so, which our .o does not link —
 * the complete binary would fail at link time (see the rejected version of this
 * idea, commit ed79db8 on branch req-isolation, which instead blocked
 * session_start() through zend_disable_functions).
 *
 * The route used here: STD_PHP_INI_ENTRY (Zend/zend_ini.h) writes into EVERY
 * module INI entry:
 *   mh_arg1 = offsetof(zend_ps_globals, <field>)
 *   mh_arg2 = in a NON-ZTS build, a pointer TO the module-global base, that is
 *             directly &ps_globals (see STD_ZEND_INI_ENTRY in zend_ini.h,
 *             the #else branch — our build is NTS, verified by the absence of
 *             --enable-maintainer-zts / --enable-zts in config.nice on the test
 *             host and by fpm_coop_validate(), which already rejects ZTS for
 *             this executor for the same reason).
 * The "session.save_path" entry (ext/session/session.c, PHP_INI_BEGIN) is
 * registered through STD_PHP_INI_ENTRY, so:
 *   zend_hash_str_find_ptr(EG(ini_directives), "session.save_path", ...)
 * returns a zend_ini_entry whose mh_arg2 is the ps_globals address. Zero linker
 * dependency — works identically when session is statically compiled and when
 * it is a .so, because the INI ENTRY TABLE itself (filled by
 * zend_register_ini_entries_ex, Zend/zend_ini.c) carries the address, not the
 * module symbol.
 *
 * Struct size: sizeof(zend_ps_globals), from the INCLUDED
 * ext/session/php_session.h. It is safe to include that header — it creates no
 * symbol dependency by itself (ext/standard/basic_functions.c includes it
 * without any guard too, is always compiled, regardless of whether session is
 * enabled). A dependency would arise only from ZEND_EXTERN_MODULE_GLOBALS(ps)
 * (declares "extern zend_ps_globals ps_globals" — see Zend/zend_API.h) OR from
 * referring to ps_globals / session_module_entry by name (both are declared in
 * this header as extern). This file does neither: it obtains the ps_globals
 * address ONLY from the INI entry's mh_arg2, and the session module (for calling
 * RINIT/RSHUTDOWN) ONLY from module_registry by the name "session" (hashed
 * lookup, not a symbol).
 *
 * Verified experimentally (not only on paper): fpm_coop_session_selfcheck()
 * below compares a field read from the calculated address with the value of the
 * same directive read through the normal INI path (zend_ini_long — the same
 * value PHP's ini_get() sees). A mismatch disables the entire path instead of
 * silently corrupting memory. The test-host log confirms agreement (see report).
 *
 * ZTS warning: in a ZTS build mh_arg2 is an *offset in TSRM* (int, not a
 * pointer) — this route would NOT work there. The project builds NTS (see
 * above), so this case is not supported; fpm_coop_validate() already rejects
 * ZTS for this executor for another reason (memcpy SG/EG by value), so we would
 * never reach this code in a ZTS build anyway.
 *
 * --- Lifecycle --------------------------------------------------------------
 *
 * Find the module in module_registry by the name "session"
 * (zend_module_entry*, fields request_startup_func/request_shutdown_func —
 * Zend/zend_modules.h). fpm_coop_session_request_startup() (called from
 * fpm_coop_req_run immediately BEFORE the script, next to creation of a fresh
 * symbol_table) copies the BASE state (captured once in
 * fpm_coop_session_container_start, AFTER the container's own RINIT — therefore
 * with correct INI values: save_path, cookie_lifetime, etc., which
 * RINIT/RSHUTDOWN do not touch because the INI system manages them) into live
 * globals, THEN calls RINIT. RINIT resets per-request fields itself
 * (php_rinit_session_globals in session.c: id=NULL, session_status=none,
 * in_save_handler=false, ...) and, if session.auto_start=1, calls
 * php_session_start() — HERE, for THIS request, so auto_start works correctly
 * PER REQUEST instead of starting one shared session for the whole process (the
 * gap that a fallback blocking session_start() would have, without this fix —
 * see the docs in commit ed79db8).
 *
 * fpm_coop_session_request_shutdown() (called from fpm_coop_req_run AFTER the
 * script, BEFORE destroying EG(symbol_table) — RSHUTDOWN calls
 * php_session_flush(), which reads $_SESSION = PS(http_session_vars), so it
 * must run before the symbol table disappears) calls RSHUTDOWN. RSHUTDOWN
 * flushes sessions to storage (I/O — fine on a Fiber; a suspension during it
 * switches through normal fpm_coop_req_leave/enter because it happens IN THE
 * REQUEST CONTEXT, the same mechanism as a suspension anywhere else in the
 * script) and releases user save-handler closures (session_set_save_handler) —
 * SESSION_FREE_USER_HANDLER in session.c.
 *
 * --- Memory ownership (swap by value, not by destructor) --------------------
 *
 * As with SG/OG/symbol_table in fpm_pool_coop.c: swap is a PURE byte memcpy,
 * without changing refcounts. This is safe while there is EXACTLY ONE logical
 * copy at a time: either in live globals (request on the processor or during
 * RINIT/RSHUTDOWN), or in ctx->session_globals (suspended request). Every
 * entry/leave moves the bytes; it never copies them to TWO places at once.
 * Closures from session_set_save_handler (stored in PS(mod_user_names)) travel
 * in the same bytes and have an owner exactly like any other zval in this swap.
 * Release: RSHUTDOWN (SESSION_FREE_USER_HANDLER) destroys them normally, INSIDE
 * live globals, BEFORE any base restore — there is no double free because the
 * base never contains "active" closures (captured once from a clean container
 * state, before any session_set_save_handler).
 */

#include "fpm_config.h"

#include <string.h>

#include "php.h"
#include "zend_API.h"
#include "zend_ini.h"
#include "zend_modules.h"
#include "ext/session/php_session.h"

#include "fpm_pool_coop.h"
#include "fpm_pool_coop_session.h"
#include "zlog.h"

static bool fpm_coop_session_ready = false;
static zend_module_entry *fpm_coop_session_mod;
static void *fpm_coop_session_globals_addr;
static unsigned char fpm_coop_session_base[sizeof(zend_ps_globals)];

/* Compare the field at the calculated address with the value of THE SAME
 * directive read through the normal INI path (zend_ini_long — what ini_get()
 * sees). A mismatch means the mh_arg2 route did not work (for example, a ZTS
 * build that should not reach this code, or a future Zend layout change) —
 * disable the path instead of writing to foreign memory. */
static bool fpm_coop_session_selfcheck(void) /* {{{ */
{
	php_ps_globals *ps = (php_ps_globals *) fpm_coop_session_globals_addr;
	zend_long ini_val = zend_ini_long(ZEND_STRL("session.cookie_lifetime"), 0);

	if (ps->cookie_lifetime != ini_val) {
		zlog(ZLOG_ALERT, "[pool %s] coop-session: SELFCHECK NIEUDANY — session.cookie_lifetime spod wyliczonego "
			"adresu ps_globals (" ZEND_LONG_FMT ") != wartosc z ini (" ZEND_LONG_FMT "); adres z wpisu ini "
			"NIE wskazuje na prawdziwe ps_globals, izolacja stanu ext/session WYLACZONA",
			fpm_coop_pool_name(), ps->cookie_lifetime, ini_val);
		return false;
	}
	return true;
}
/* }}} */

void fpm_coop_session_container_start(void) /* {{{ */
{
	zend_ini_entry *entry;
	zend_module_entry *mod;

	if (zend_get_module_started("session") != SUCCESS) {
		/* Module not loaded (--disable-session or the request script does not
		 * use it) — disable this path; the remaining hooks cost one "if". */
		return;
	}

	entry = zend_hash_str_find_ptr(EG(ini_directives), ZEND_STRL("session.save_path"));
	if (!entry || !entry->mh_arg2) {
		zlog(ZLOG_WARNING, "[pool %s] coop-session: modul session zaladowany, ale brak dzialajacego wpisu ini "
			"'session.save_path' — punkt zaczepienia nie zadzialal, izolacja stanu ext/session WYLACZONA "
			"(session_start() bedzie dzialac tylko przy jednym requescie w locie)",
			fpm_coop_pool_name());
		return;
	}

	mod = zend_hash_str_find_ptr(&module_registry, ZEND_STRL("session"));
	if (!mod || !mod->request_startup_func || !mod->request_shutdown_func) {
		zlog(ZLOG_WARNING, "[pool %s] coop-session: modul session bez RINIT/RSHUTDOWN w module_registry — "
			"izolacja stanu ext/session WYLACZONA", fpm_coop_pool_name());
		return;
	}

	fpm_coop_session_globals_addr = entry->mh_arg2;
	fpm_coop_session_mod = mod;

	if (!fpm_coop_session_selfcheck()) {
		fpm_coop_session_globals_addr = NULL;
		fpm_coop_session_mod = NULL;
		return;
	}

	/* State AFTER the container's own RINIT (fpm_coop_container_start in
	 * fpm_pool_coop.c calls us exactly here): correct INI values (save_path,
	 * cookie_lifetime, ...), no active session, no user save handler. This is the
	 * state from which every new request safely starts RINIT. */
	memcpy(fpm_coop_session_base, fpm_coop_session_globals_addr, sizeof(fpm_coop_session_base));
	fpm_coop_session_ready = true;

	zlog(ZLOG_NOTICE, "[pool %s] coop-session: izolacja stanu ext/session per request WLACZONA "
		"(punkt zaczepienia: wpis ini 'session.save_path', modul '%s')",
		fpm_coop_pool_name(), mod->name);
}
/* }}} */

void fpm_coop_session_req_enter(struct fpm_coop_req_s *ctx) /* {{{ */
{
	if (!fpm_coop_session_ready) {
		return;
	}
	memcpy(fpm_coop_session_globals_addr, ctx->session_globals, sizeof(ctx->session_globals));
}
/* }}} */

void fpm_coop_session_req_save(struct fpm_coop_req_s *ctx) /* {{{ */
{
	if (!fpm_coop_session_ready) {
		return;
	}
	memcpy(ctx->session_globals, fpm_coop_session_globals_addr, sizeof(ctx->session_globals));
}
/* }}} */

void fpm_coop_session_base_restore(void) /* {{{ */
{
	if (!fpm_coop_session_ready) {
		return;
	}
	memcpy(fpm_coop_session_globals_addr, fpm_coop_session_base, sizeof(fpm_coop_session_base));
}
/* }}} */

void fpm_coop_session_request_startup(void) /* {{{ */
{
	if (!fpm_coop_session_ready) {
		return;
	}
	memcpy(fpm_coop_session_globals_addr, fpm_coop_session_base, sizeof(fpm_coop_session_base));
	if (fpm_coop_session_mod->request_startup_func(fpm_coop_session_mod->type, fpm_coop_session_mod->module_number) == FAILURE) {
		zlog(ZLOG_WARNING, "[pool %s] coop-session: RINIT modulu session nie powiodlo sie", fpm_coop_pool_name());
	}
}
/* }}} */

void fpm_coop_session_request_shutdown(void) /* {{{ */
{
	if (!fpm_coop_session_ready) {
		return;
	}
	zend_try {
		fpm_coop_session_mod->request_shutdown_func(fpm_coop_session_mod->type, fpm_coop_session_mod->module_number);
	} zend_end_try();
}
/* }}} */
