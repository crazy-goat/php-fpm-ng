/* fpm-ng: the fiber/async executor variants. See fpm_pool_type_coop.h for
 * why this is a separate file from fpm_pool_type.c and what stays stable
 * across a build with either flag, both, or neither.
 */

#include "fpm_config.h"

#include <string.h>

#include "fpm.h"
#include "fpm_pool_type.h"
#include "fpm_pool_type_coop.h"
#include "fpm_http.h"
#include "fpm_pool_async.h"
#include "fpm_pool_coop.h"
#include "fpm_pool_coop_statics.h"
#include "fpm_pool_fiber.h"

#ifdef HAVE_FPMNG_FIBER
static int fpm_pool_type_coop_fiber_validate(struct fpm_worker_pool_s *wp)
{
	if (fpm_coop_validate(wp, "fiber") < 0) {
		return -1;
	}
	/* fiber.isolate_statics syntax check -- master side, before any fork.
	 * See fpm_pool_coop_statics.c: class/property existence cannot be checked
	 * here (no autoloader yet), only at runtime. */
	return fpm_coop_statics_validate(wp);
}
#endif

#if defined(HAVE_FPMNG_FIBER) || defined(HAVE_FPMNG_ASYNC)
static int fpm_pool_type_coop_http_concurrent_init(struct fpm_worker_pool_s *wp)
{
	/* A multi-request executor can handle multiple connections per worker. */
	return fpm_http_init_pool_with_capacity(wp, 128);
}
#endif

/* Both groups below exist only in a binary built with the corresponding flag
 * (--enable-fpmng-fiber / --enable-fpmng-async, both default "no"). Without
 * the flag the sources are not compiled at all (see build/prepare.sh and
 * sapi/fpmng/config.m4), so these structures are protected by the same
 * #ifdef -- fpm_pool_type_coop_variant() below hands back NULL for them
 * instead. */
#ifdef HAVE_FPMNG_FIBER
static const struct fpm_pool_type_s fpm_pool_http_fiber = {
	.name                         = "http",
	/* Issue #295. Experimental, and the tracker is the argument: #79, #80,
	 * #82, #84 and #85 are open correctness bugs against this executor's
	 * request isolation, and criterion 3 of the bar in #269 ("no open
	 * correctness issue") is therefore not met. It is also behind a
	 * default-off configure flag, so nobody is running it by accident. */
	.tier                         = FPM_TIER_EXPERIMENTAL,
	.requires_listen              = 1,
	.requires_pm                  = 1,
	.serves_requests              = 1,
	.reuses_request_runtime       = 1,
	.listening_socket_nonblocking = 1,
	.baseline_counter             = "requests",
	.operator_endpoint            = 1,
	.rejects                      = fpm_coop_rejects,
	.validate                     = fpm_pool_type_coop_fiber_validate,
	.init_main                    = fpm_pool_type_coop_http_concurrent_init,
	.child_main                   = fpm_pool_fiber_child_main,
};
#endif /* HAVE_FPMNG_FIBER */

#ifdef HAVE_FPMNG_ASYNC
static const struct fpm_pool_type_s fpm_pool_http_async = {
	.name                   = "http",
	/* Issue #295. Experimental, one criterion short of beta in a way that is
	 * cheap to state: no cell in CI builds --enable-fpmng-async at all (see
	 * build-matrix.yml and issue #87), so criterion 1 of #269's bar -- tests
	 * on every PR -- has nothing behind it here. */
	.tier                   = FPM_TIER_EXPERIMENTAL,
	.requires_listen        = 1,
	.requires_pm            = 1,
	.serves_requests        = 1,
	.reuses_request_runtime = 1,
	.baseline_counter       = "requests",
	.operator_endpoint      = 1,
	.rejects                = fpm_pool_async_rejects,
	.validate               = fpm_pool_async_validate,
	.init_main              = fpm_pool_type_coop_http_concurrent_init,
	.child_main             = fpm_pool_async_child_main,
};
#endif /* HAVE_FPMNG_ASYNC */

const struct fpm_pool_type_s *fpm_pool_type_coop_variant(const char *type_name, const char *executor_name)
{
#ifdef HAVE_FPMNG_FIBER
	if (!strcmp(executor_name, "fiber") && !strcmp(type_name, "http")) {
		return &fpm_pool_http_fiber;
	}
#endif
#ifdef HAVE_FPMNG_ASYNC
	if (!strcmp(executor_name, "async") && !strcmp(type_name, "http")) {
		return &fpm_pool_http_async;
	}
#endif
	(void) type_name;
	(void) executor_name;
	return NULL;
}
