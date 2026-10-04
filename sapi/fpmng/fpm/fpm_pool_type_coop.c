/* fpm-ng: the fiber executor variant. See fpm_pool_type_coop.h for
 * why this is a separate file from fpm_pool_type.c and what stays stable
 * across a build with either flag, both, or neither.
 */

#include "fpm_config.h"

#include <string.h>

#include "fpm.h"
#include "fpm_pool_type.h"
#include "fpm_pool_type_coop.h"
#include "fpm_pool_coop.h"
#include "fpm_pool_coop_statics.h"
#include "fpm_pool_fiber.h"
#include "fpm_pool_fiber_intercept.h"

#ifdef HAVE_FPMNG_FIBER
static int fpm_pool_type_coop_fiber_validate(struct fpm_worker_pool_s *wp)
{
	if (fpm_coop_validate(wp, "fiber") < 0) {
		return -1;
	}
	/* fiber.disable_interceptions: every name must be a registry entry --
	 * master side, so a typo fails the start instead of silently leaving the
	 * interception on (fpm_pool_fiber_intercept.c). */
	if (fpm_fiber_intercept_validate(wp) < 0) {
		return -1;
	}
	/* fiber.isolate_statics syntax check -- master side, before any fork.
	 * See fpm_pool_coop_statics.c: class/property existence cannot be checked
	 * here (no autoloader yet), only at runtime. */
	return fpm_coop_statics_validate(wp);
}
#endif

/* The group below exists only in a binary built with the flag
 * (--enable-fpmng-fiber, default "no"). Without
 * the flag the sources are not compiled at all (see build/prepare.sh and
 * sapi/fpmng/config.m4), so these structures are protected by the same
 * #ifdef -- fpm_pool_type_coop_variant() below hands back NULL for them
 * instead. */
#ifdef HAVE_FPMNG_FIBER
static const struct fpm_pool_type_s fpm_pool_fastcgi_fiber = {
	.name = "fastcgi",
	/* Issue #295. Experimental, and the tracker is the argument: #79, #80,
	 * #82, #84 and #85 are open correctness bugs against this executor's
	 * request isolation, and criterion 3 of the bar in #269 ("no open
	 * correctness issue") is therefore not met. It is also behind a
	 * default-off configure flag, so nobody is running it by accident. */
	.tier = FPM_TIER_EXPERIMENTAL,
	.requires_listen = 1,
	.requires_pm = 1,
	.serves_requests = 1,
	.serves_fastcgi = 1,
	.listening_socket_nonblocking = 1,
	.baseline_counter = "requests",
	.operator_endpoint = 1,
	.rejects = fpm_coop_rejects,
	.validate = fpm_pool_type_coop_fiber_validate,
	.child_main = fpm_pool_fiber_child_main,
	/* What the gateway sizes its upstream budget towards this pool by, per
	 * child. The number is the one the first http-fiber POC (e441814) gave
	 * its gateway, there for the whole pool and overridable through
	 * FPM_HTTP_MAX_UPSTREAMS; it was never measured, and the child itself
	 * does not enforce it. */
	.requests_per_child = 128,
};
#endif /* HAVE_FPMNG_FIBER */

const struct fpm_pool_type_s *fpm_pool_type_coop_variant(const char *type_name, const char *executor_name)
{
#ifdef HAVE_FPMNG_FIBER
	if (!strcmp(executor_name, "fiber") && !strcmp(type_name, "fastcgi")) {
		return &fpm_pool_fastcgi_fiber;
	}
#endif
	(void) type_name;
	(void) executor_name;
	return NULL;
}
