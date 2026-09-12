/* The one php-src symbol the libphp build cannot link, and the stand-in for it
 * (issue #213, from spike #198 -- docs/spike-libphp-link-report.md).
 *
 * This file is compiled in every build and is empty in all but one of them. It
 * has content only under FPMNG_LIBPHP_BUILD, which build/libphp-build.sh sets
 * and configure never does: that build links against a distribution's
 * libphp.so instead of compiling php-src, so that `pool.type = fastcgi` and
 * `pool.type = http-direct` can ship as a package with no compiler on the
 * user's side (issue #212).
 *
 * WHY THE SYMBOL IS MISSING. Zend/zend_signal.h:94 declares zend_signal_init()
 * without ZEND_API, and PHP is built with -fvisibility=hidden, so it is not in
 * the shared object's dynamic symbol table. It is not deprecated and not
 * internal by intent -- it is simply a function upstream had no reason to
 * export, because inside php-src the caller and the callee are in the same
 * link.
 *
 * WHY IT MATTERS. FPM calls it from fpm_signals_init_child()
 * (sapi/fpmng/fpm/fpm_signals.c), to snapshot the child's signal handlers into
 * the file-static global_orig_handlers, and a second time from an http-direct
 * classic child (fpm_http_direct.c, issue #256), which installs its own
 * SIGQUIT and SIGUSR1 handlers after that first snapshot was taken and needs
 * the snapshot redone over them. zend_signal_activate() copies that
 * snapshot into SIGG(handlers) at the start of every request. The child
 * installs its own SIGQUIT/SIGTERM handlers before that point, so without the
 * snapshot each request restores the MASTER's handlers and a graceful stop
 * lands in the wrong place. The file-static cannot be written from outside the
 * library, so there is no way to do this except through the library's own
 * code.
 *
 * WHY zend_signal_startup() IS A VALID STAND-IN. It IS exported, and its last
 * statement is a call to zend_signal_init() (Zend/zend_signal.c:443). What it
 * does on top is zend_signal_globals_ctor() -- a memset of the globals, reset
 * = 1, and a rebuild of the pending-queue free list -- and a recomputation of
 * global_sigmask. All of that is idempotent at both points FPM calls this: the
 * child has forked and has not run a request, so the globals still hold
 * exactly the post-ctor state the master left them in. Calling it again writes
 * back the values that are already there. A call after a request has run would
 * not be safe -- it would clear SIGG(active) and drop the pending queue -- and
 * neither caller is in that position.
 *
 * NOT VALID UNDER ZTS. There zend_signal_globals_ctor() is reached through
 * ts_allocate_fast_id(), and calling it twice allocates a second thread-local
 * id for the same globals -- every later access would then read a different
 * copy than the one the engine writes. Both the distribution builds this
 * targets and ours are NTS. build/libphp-build.sh refuses to build against a
 * ZTS libphp for this reason, naming this file; the #error below is the second
 * line of defence, for the case where someone drives the compile by hand.
 *
 * THE CONTRAST IS TESTED. Spike #198 measured the counterfactual on this exact
 * path: with an empty body here the owned suite loses
 * fpmng-http-direct-lifecycle.phpt, where a graceful stop truncates the
 * in-flight request. That test is the guard on this file -- if this stand-in
 * ever stops being equivalent, that is where it shows.
 */

#ifdef FPMNG_LIBPHP_BUILD

#include "php.h"
#include "zend_API.h"
#include "zend_signal.h"
#include "php_fpmng_metrics.h"
#include "zlog.h"

#ifdef ZTS
# error "the libphp build substitutes zend_signal_init() with zend_signal_startup(), which is not idempotent under ZTS; build against an NTS libphp or build from source"
#endif

void zend_signal_init(void)
{
	zend_signal_startup();
}

/* ext/fpmng_metrics on the libphp path (issue #216).
 *
 * The C side of the extension is linked into this binary either way -- fpm_metrics.c
 * calls into it, so `pool.type = status` renders /metrics on both builds. What the
 * libphp build does not get for free is the PHP MODULE: in a from-source build
 * configure puts the extension in the static module list main/internal_functions.c
 * writes, and php_module_startup() walks that list. Here the list belongs to the
 * distribution's libphp, which has never heard of us, so fpm_metric_register() and
 * the other four userland functions would simply not exist -- and nothing would say
 * so. A "fast check" binary with fewer PHP-visible functions than the real one is
 * the kind of difference that makes people stop trusting the fast check.
 *
 * The module also owns the fpmng_metrics.series_limit INI entry, which
 * fpm_metrics_init_main() reads to size the shared memory region. Without this
 * registration that read returns a zeroed globals struct and the limit clamps to 1,
 * so the libphp build reserved room for a single series per worker.
 *
 * Registering after php_module_startup() is the documented way in: it is what dl()
 * does. zend_startup_module() registers the entry and runs its MINIT, and the call
 * happens in the master before any child is forked, so every child inherits it.
 *
 * WHY THE RESULT IS CHECKED. A failing MINIT is loud -- zend_startup_module_ex()
 * turns it into E_CORE_ERROR and does not return. The other failure mode is not:
 * if zend_register_functions() rejects the function set, because a distribution's
 * libphp already exports one of the fpm_metric_* names, the engine emits a single
 * E_CORE_WARNING and hands back NULL. Started that way the daemon looks healthy:
 * /metrics renders, but the INI entry is missing, the shm holds one series, and the
 * operator sees "series limit exhausted" instead of the real cause. Refusing to
 * start says it once, at the point where it can still be read.
 *
 * This does NOT make the module appear in `php-fpm-ng -m` or in `php-fpm-ng -i`:
 * both print and exit without ever reaching fpm_init(). The probe that matters is a
 * request -- sapi/fpmng/tests/fpmng-metrics-userland.phpt asks a running pool
 * whether the functions are there, which is the question a user actually has.
 */
int fpmng_libphp_register_bundled_modules(void)
{
	if (zend_hash_str_exists(&module_registry, "fpmng_metrics", sizeof("fpmng_metrics") - 1)) {
		return 0;
	}

	if (zend_startup_module(&fpmng_metrics_module_entry) == FAILURE) {
		zlog(ZLOG_ERROR, "could not register the fpmng_metrics extension against this libphp; "
				"see the E_CORE_WARNING above for what the engine rejected");
		return -1;
	}

	return 0;
}

#else

/* The from-source build links the real zend_signal_init(), and configure has
 * already put ext/fpmng_metrics in the static module list, so both jobs of this
 * file are done elsewhere. The no-op keeps the call site in fpm_init() free of
 * an #ifdef -- the caller asks the same question in both builds and one of the
 * two answers is "nothing to do". */
int fpmng_libphp_register_bundled_modules(void)
{
	return 0;
}

#endif
