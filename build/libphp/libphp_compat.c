/* The two symbols a distribution libphp does not export (issue #212, from
 * spike #198). Both are needed only on the libphp build path; the from-source
 * build has the real ones.
 *
 * 1. zend_signal_init() is declared without ZEND_API (Zend/zend_signal.h:94)
 *    and PHP is built with -fvisibility=hidden, so it is absent from the .so's
 *    dynamic symbol table. FPM calls it once per child, from
 *    fpm_signals_init_child() (sapi/fpm/fpm/fpm_signals.c:252), to snapshot the
 *    child's signal handlers into the file-static global_orig_handlers, which
 *    zend_signal_activate() copies into SIGG(handlers) at the start of every
 *    request. That file-static cannot be written from outside the library.
 *
 *    zend_signal_startup() IS exported, and its last statement is a call to
 *    zend_signal_init() (Zend/zend_signal.c:443). What it adds on top --
 *    zend_signal_globals_ctor() (memset of the globals, reset = 1, rebuild of
 *    the pending-queue free list) and recomputing global_sigmask -- is
 *    idempotent at the point FPM needs the snapshot: the child has not run a
 *    request yet, so the globals still hold exactly the post-ctor state the
 *    master left them in.
 *
 *    This is NOT VALID UNDER ZTS: there zend_signal_startup() calls
 *    ts_allocate_fast_id(), and a second call is a bug. Both distribution
 *    builds and ours are NTS.
 *
 *    The snapshot is genuinely load-bearing and the owned suite catches its
 *    absence. Spike #198 measured the counterfactual: with an empty
 *    zend_signal_init() the suite reports 47 PASS / 2 FAIL / 10 SKIP instead of
 *    48/1/10, and the extra failure is fpmng-http-direct-lifecycle.phpt --
 *    a graceful stop truncates the in-flight request, because what
 *    zend_signal_activate() restores is not the child's sig_soft_quit handler.
 *    Making this a first-class seam rather than a substitution is issue #213.
 *
 * 2. zend_signal_use_persistent_handlers() (patches/0006, inside Zend/) USED to
 *    be substituted here with a no-op so the link would close. It is not, any
 *    more: a no-op is upstream signal behaviour wearing the name of the
 *    opposite, and nothing at run time would have said so. This build now
 *    leaves HAVE_FPMNG_PERSISTENT_SIGNALS unset instead, which compiles the
 *    call out of fpm.c and makes every pool type that depends on it refuse to
 *    start with a named reason (issue #214,
 *    fpm_pool_type_check_build_support()). One fact, one define, one place to
 *    change when the missing patch arrives -- not a stub to remember to
 *    delete.
 */
#include "php.h"
#include "zend_signal.h"

void zend_signal_init(void)
{
	zend_signal_startup();
}
