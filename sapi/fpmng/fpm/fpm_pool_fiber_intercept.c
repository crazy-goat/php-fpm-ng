/* fpm-ng: pool.executor = fiber — the interception registry. See
 * fpm_pool_fiber_intercept.h and docs/fiber_async_io.md, "The IO seam".
 *
 * The switch is a pool directive, fiber.disable_interceptions = a, b, rather
 * than a build flag or a php.ini setting:
 * - not a build flag, because the point is to turn one interception off
 *   without a rebuild when it misbehaves in production (issue #531, curl);
 * - not php.ini, because interceptions are installed once per child before
 *   any request runs and stay for the life of the process: a per-request
 *   ini_set() could not undo them, and an INI_SYSTEM entry would apply to
 *   every pool of the binary while one php-fpm-ng runs several pools;
 * - a pool directive is validated in the master before any fork, names the
 *   pool in its error, is already rejected on every other executor (the
 *   "fiber." prefix, fpm_pool_type.c), and reaches the child through
 *   wp->config like fiber.revalidate_freq does.
 * Disabling is enforced twice: the entry is not installed (the function table,
 * the stream ops and the transports stay stock), and the seam answers
 * UNSUPPORTED to it (fpm_fiber_io_can_suspend), which is what turns off the
 * call sites compiled into php-src by patches 0007 and 0008.
 */

#include "fpm_config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_fiber_io.h"
#include "fpm_pool_fiber_intercept.h"
#include "fpm_pool_fiber_flock.h"
#include "fpm_pool_fiber_select.h"
#include "fpm_pool_fiber_sleep.h"
#include "fpm_pool_fiber_xport.h"
#include "zlog.h"

/* Install order. Every install runs in the child after php_module_startup()
 * (fpm_main.c: startup() before fpm_run()), so after every extension's MINIT.
 *
 * - xport FIRST and after ext/openssl's MINIT: it reads the "tcp" factory that
 *   ext/openssl registers over the generic one, wraps it, and (under
 *   HAVE_FPMNG_FIBER_TLS) re-arms ssl/tls with patch 0007's factories. Covers
 *   tcp/unix/TLS streams and the DNS lookup inside connect.
 * - flock: wraps php_stream_stdio_ops.set_option in place so a lock held by
 *   ANOTHER Fiber of this process suspends instead of blocking the event loop
 *   in the kernel (docs/flock-streams-spike-report.md,
 *   docs/flock-fiber-deadlock-report.md); independent of the transports, same
 *   "after MINIT" timing.
 * - sleep: swaps three zif_handlers in CG(function_table); order relative to
 *   the others does not matter (different tables).
 * - select: nothing to install, patch 0008's call site is compiled in; the
 *   entry exists so that the switch applies to it.
 *
 * curl, when it exists, is one more line here. */
static struct fpm_fiber_intercept_s *const fpm_fiber_intercepts[] = {
	&fpm_fiber_xport_intercept,
	&fpm_fiber_flock_intercept,
	&fpm_fiber_sleep_intercept,
	&fpm_fiber_select_intercept,
	NULL
};

/* Calls cb for every name in a comma/whitespace separated list; stops at the
 * first non-zero return and hands it back. */
static int fpm_fiber_intercept_foreach_name(const char *list, int (*cb)(const char *name, size_t len, void *arg), void *arg) /* {{{ */
{
	const char *p = list;

	if (!p) {
		return 0;
	}
	while (*p) {
		const char *start;
		int ret;

		while (*p == ',' || isspace((unsigned char) *p)) {
			p++;
		}
		if (!*p) {
			break;
		}
		start = p;
		while (*p && *p != ',' && !isspace((unsigned char) *p)) {
			p++;
		}
		ret = cb(start, (size_t) (p - start), arg);
		if (ret) {
			return ret;
		}
	}
	return 0;
}
/* }}} */

static struct fpm_fiber_intercept_s *fpm_fiber_intercept_find(const char *name, size_t len) /* {{{ */
{
	int i;

	for (i = 0; fpm_fiber_intercepts[i]; i++) {
		if (strlen(fpm_fiber_intercepts[i]->name) == len && !strncmp(fpm_fiber_intercepts[i]->name, name, len)) {
			return fpm_fiber_intercepts[i];
		}
	}
	return NULL;
}
/* }}} */

static int fpm_fiber_intercept_validate_cb(const char *name, size_t len, void *arg) /* {{{ */
{
	struct fpm_worker_pool_s *wp = arg;
	char known[128];
	size_t off = 0;
	int i;

	if (fpm_fiber_intercept_find(name, len)) {
		return 0;
	}
	known[0] = '\0';
	for (i = 0; fpm_fiber_intercepts[i] && off < sizeof(known); i++) {
		int n = snprintf(known + off, sizeof(known) - off, "%s%s", i ? ", " : "", fpm_fiber_intercepts[i]->name);

		if (n < 0) {
			break;
		}
		off += (size_t) n;
	}
	zlog(ZLOG_ALERT, "[pool %s] fiber.disable_interceptions: unknown interception '%.*s'; known interceptions: %s",
		wp->config->name, (int) len, name, known);
	return -1;
}
/* }}} */

int fpm_fiber_intercept_validate(struct fpm_worker_pool_s *wp) /* {{{ */
{
	return fpm_fiber_intercept_foreach_name(wp->config->fiber_disable_interceptions, fpm_fiber_intercept_validate_cb, wp) ? -1 : 0;
}
/* }}} */

static int fpm_fiber_intercept_disable_cb(const char *name, size_t len, void *arg) /* {{{ */
{
	struct fpm_fiber_intercept_s *e = fpm_fiber_intercept_find(name, len);

	(void) arg;
	if (e) {	/* validate() already refused unknown names in the master */
		e->disabled = true;
	}
	return 0;
}
/* }}} */

void fpm_fiber_intercept_install_all(struct fpm_worker_pool_s *wp) /* {{{ */
{
	int i;

	fpm_fiber_intercept_foreach_name(wp->config->fiber_disable_interceptions, fpm_fiber_intercept_disable_cb, NULL);

	for (i = 0; fpm_fiber_intercepts[i]; i++) {
		struct fpm_fiber_intercept_s *e = fpm_fiber_intercepts[i];

		if (e->disabled) {
			zlog(ZLOG_NOTICE, "[pool %s] fiber: interception '%s' disabled by fiber.disable_interceptions; "
				"its calls block the whole process, as on stock PHP", wp->config->name, e->name);
			if (e->disabled_hazard) {
				zlog(ZLOG_WARNING, "[pool %s] fiber: interception '%s' disabled: %s", wp->config->name, e->name, e->disabled_hazard);
			}
			continue;
		}
		if (e->install) {
			e->install();
		}
		zlog(ZLOG_DEBUG, "[pool %s] fiber: interception '%s' installed", wp->config->name, e->name);
	}
}
/* }}} */

void fpm_fiber_intercept_request_end(void *waker) /* {{{ */
{
	int i;

	for (i = 0; fpm_fiber_intercepts[i]; i++) {
		if (!fpm_fiber_intercepts[i]->disabled && fpm_fiber_intercepts[i]->request_end) {
			fpm_fiber_intercepts[i]->request_end(waker);
		}
	}
}
/* }}} */
