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
 * WHY IT MATTERS. FPM calls it once per child, from fpm_signals_init_child()
 * (sapi/fpmng/fpm/fpm_signals.c), to snapshot the child's signal handlers into
 * the file-static global_orig_handlers. zend_signal_activate() copies that
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
 * global_sigmask. All of that is idempotent at the only point FPM calls this:
 * the child has just forked and has not run a request, so the globals still
 * hold exactly the post-ctor state the master left them in. Calling it again
 * writes back the values that are already there.
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
#include "zend_signal.h"

#ifdef ZTS
# error "the libphp build substitutes zend_signal_init() with zend_signal_startup(), which is not idempotent under ZTS; build against an NTS libphp or build from source"
#endif

void zend_signal_init(void)
{
	zend_signal_startup();
}

#else

/* The from-source build links the real zend_signal_init() and needs nothing
 * from here. ISO C forbids an empty translation unit, and -pedantic would say
 * so, hence the declaration. */
typedef int fpmng_libphp_compat_not_needed_here;

#endif
