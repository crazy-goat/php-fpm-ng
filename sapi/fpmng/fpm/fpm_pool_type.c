/* fpm-ng: pool-type registry. See fpm_pool_type.h. */

#include "fpm_config.h"

#include <string.h>
#include <stdio.h>
#include <fcntl.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_type.h"
#include "fpm_http.h"
#include "fpm_pool_supervisor.h"
#include "fpm_pool_cron.h"
#include "fpm_pool_status.h"
#include "fpm_pool_async.h"
#include "fpm_pool_coop.h"
#include "fpm_pool_coop_statics.h"
#include "fpm_pool_fiber.h"
#include "fpm_scoreboard.h"
#include "zlog.h"

/* http.* tunes the gateway, which starts only under pool.type = http — on every
 * other type these directives have nothing to tune. fiber.* applies only to
 * pool.executor = fiber (fpm_coop_rejects does not include it). */
static const char *const fpm_pool_fastcgi_rejects[] = {
	"http.",
	"fiber.",
	NULL
};

static const char *const fpm_pool_http_classic_rejects[] = {
	"fiber.",
	NULL
};

static int fpm_pool_type_http_init(struct fpm_worker_pool_s *wp)
{
	return fpm_http_init_pool(wp);
}

#ifdef HAVE_FPMNG_FIBER
static int fpm_pool_type_fiber_validate(struct fpm_worker_pool_s *wp)
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
static int fpm_pool_type_http_concurrent_init(struct fpm_worker_pool_s *wp)
{
	/* A multi-request executor can handle multiple connections per worker. */
	return fpm_http_init_pool_with_capacity(wp, 128);
}
#endif

/* Types visible in configuration. fastcgi-ng is the optimized FastCGI path;
 * http starts the built-in gateway. Both default to the classic executor, and
 * fpm_pool_type_resolve() selects their effective variant. */
static const struct fpm_pool_type_s fpm_pool_types[] = {
	{
		.name            = "fastcgi",
		.requires_listen = 1,
		.requires_pm     = 1,
		.serves_requests = 1,
		.rejects         = fpm_pool_fastcgi_rejects,
	},
	{
		.name            = "fastcgi-ng",
		.requires_listen = 1,
		.requires_pm     = 1,
		.serves_requests = 1,
		.rejects         = fpm_pool_fastcgi_rejects,
	},
	{
		.name            = "http",
		.requires_listen = 1,
		.requires_pm     = 1,
		.serves_requests = 1,
		.rejects         = fpm_pool_http_classic_rejects,
		.validate        = fpm_http_validate_pool,
		.init_main       = fpm_pool_type_http_init,
	},
	{
		.name            = "supervisor",
		.requires_listen = 0,
		.requires_pm     = 1,	/* pm.* is generated from supervisor.processes; see fpm_pool_supervisor.c */
		.serves_requests = 0,
		.rejects         = fpm_pool_supervisor_rejects,
		.validate        = fpm_pool_supervisor_validate,
		.init_main       = fpm_pool_supervisor_init_main,
		.child_main      = fpm_pool_supervisor_child_main,
		.status          = fpm_pool_supervisor_status,
	},
	{
		.name            = "cron",
		.requires_listen = 0,
		.requires_pm     = 0,	/* validate() always sets pm=static+max_children=1 programmatically */
		.serves_requests = 0,
		.rejects         = fpm_pool_cron_rejects,
		.validate        = fpm_pool_cron_validate,
		.init_main       = fpm_pool_cron_init_main,
		.child_main      = fpm_pool_cron_child_main,
		.status          = fpm_pool_cron_status,
	},
	{
		.name                     = "status",
		.requires_listen          = 1,	/* own HTTP port, directly */
		.requires_pm              = 0,	/* validate() always sets pm=static+max_children=1 programmatically */
		.serves_requests          = 0,
		.reads_foreign_scoreboards = 1,
		.rejects                  = fpm_pool_status_rejects,
		.validate                 = fpm_pool_status_validate,
		.child_main               = fpm_pool_status_child_main,
	},
};

/* Effective type/executor combinations. They are not separate pool.type
 * values and therefore do not appear in the type list in messages.
 *
 * Both groups below exist only in a binary built with the corresponding flag
 * (--enable-fpmng-fiber / --enable-fpmng-async, both default "no"). Without
 * the flag the sources are not compiled at all (see build/prepare.sh and
 * sapi/fpmng/config.m4), so these structures and their use below in
 * fpm_pool_type_resolve() are protected by the same #ifdef. */
#ifdef HAVE_FPMNG_FIBER
static const struct fpm_pool_type_s fpm_pool_fastcgi_ng_fiber = {
	.name                         = "fastcgi-ng",
	.requires_listen              = 1,
	.requires_pm                  = 1,
	.serves_requests              = 1,
	.listening_socket_nonblocking = 1,
	.rejects                      = fpm_coop_rejects,
	.validate                     = fpm_pool_type_fiber_validate,
	.child_main                   = fpm_pool_fiber_child_main,
};

static const struct fpm_pool_type_s fpm_pool_http_fiber = {
	.name                         = "http",
	.requires_listen              = 1,
	.requires_pm                  = 1,
	.serves_requests              = 1,
	.listening_socket_nonblocking = 1,
	.rejects                      = fpm_coop_rejects,
	.validate                     = fpm_pool_type_fiber_validate,
	.init_main                    = fpm_pool_type_http_concurrent_init,
	.child_main                   = fpm_pool_fiber_child_main,
};
#endif /* HAVE_FPMNG_FIBER */

#ifdef HAVE_FPMNG_ASYNC
static const struct fpm_pool_type_s fpm_pool_fastcgi_ng_async = {
	.name            = "fastcgi-ng",
	.requires_listen = 1,
	.requires_pm     = 1,
	.serves_requests = 1,
	.rejects         = fpm_pool_async_rejects,
	.validate        = fpm_pool_async_validate,
	.child_main      = fpm_pool_async_child_main,
};

static const struct fpm_pool_type_s fpm_pool_http_async = {
	.name            = "http",
	.requires_listen = 1,
	.requires_pm     = 1,
	.serves_requests = 1,
	.rejects         = fpm_pool_async_rejects,
	.validate        = fpm_pool_async_validate,
	.init_main       = fpm_pool_type_http_concurrent_init,
	.child_main      = fpm_pool_async_child_main,
};
#endif /* HAVE_FPMNG_ASYNC */

int fpm_pool_type_check_directives(struct fpm_worker_pool_s *wp, const struct fpm_pool_type_s *type)
{
	const char *const *reject;
	int bad = 0;

	if (!type->rejects || !wp->config->set_directives) {
		return 0;
	}

	for (reject = type->rejects; *reject; reject++) {
		size_t len = strlen(*reject);

		if (len && (*reject)[len - 1] == '.') {
			/* prefix: "pm." matches every pm.* directive that was actually set */
			const char *p = wp->config->set_directives;
			char needle[128];

			if ((size_t)snprintf(needle, sizeof(needle), ";%s", *reject) >= sizeof(needle)) {
				continue;
			}
			while ((p = strstr(p, needle)) != NULL) {
				const char *end = strchr(p + 1, ';');

				zlog(ZLOG_ALERT, "[pool %s] '%.*s' is not supported by pool.type = %s",
					wp->config->name, end ? (int)(end - p - 1) : 0, p + 1, type->name);
				bad = 1;
				p = end ? end : p + strlen(p);
			}
		} else if (fpm_conf_directive_was_set(wp->config, *reject)) {
			zlog(ZLOG_ALERT, "[pool %s] '%s' is not supported by pool.type = %s",
				wp->config->name, *reject, type->name);
			bad = 1;
		}
	}

	return bad ? -1 : 0;
}

#define FPM_POOL_TYPE_COUNT (sizeof(fpm_pool_types) / sizeof(fpm_pool_types[0]))
#define FPM_POOL_TYPE_DEFAULT (&fpm_pool_types[0])

const struct fpm_pool_type_s *fpm_pool_type_get(const char *name)
{
	size_t i;

	if (!name || !*name) {
		return FPM_POOL_TYPE_DEFAULT;
	}

	/* Explicit "fcgi" was accepted before the name changed to "fastcgi". */
	if (!strcmp(name, "fcgi")) {
		return FPM_POOL_TYPE_DEFAULT;
	}

	for (i = 0; i < FPM_POOL_TYPE_COUNT; i++) {
		if (!strcmp(fpm_pool_types[i].name, name)) {
			return &fpm_pool_types[i];
		}
	}

	return NULL;
}

void fpm_pool_type_list(char *buf, size_t len)
{
	size_t i, off = 0;

	if (!len) {
		return;
	}
	buf[0] = '\0';

	for (i = 0; i < FPM_POOL_TYPE_COUNT && off + 1 < len; i++) {
		int n = snprintf(buf + off, len - off, "%s%s",
			i ? ", " : "", fpm_pool_types[i].name);
		if (n < 0 || (size_t)n >= len - off) {
			break;
		}
		off += (size_t)n;
	}
}

const struct fpm_pool_type_s *fpm_pool_type_resolve(struct fpm_worker_pool_s *wp)
{
	const struct fpm_pool_type_s *type = fpm_pool_type_get(wp->config->type);
	const char *executor = wp->config->executor;

	if (!type) {
		return NULL;
	}

	if (strcmp(type->name, "fastcgi-ng") != 0 && strcmp(type->name, "http") != 0) {
		return (!executor || !*executor) ? type : NULL;
	}

	if (!executor || !*executor || !strcmp(executor, "classic")) {
		return type;
	}
	if (!strcmp(executor, "fiber")) {
#ifdef HAVE_FPMNG_FIBER
		return !strcmp(type->name, "http") ? &fpm_pool_http_fiber : &fpm_pool_fastcgi_ng_fiber;
#else
		/* Unreachable in practice: fpm_pool_type_validate_executor() already
		 * refuses this build/executor combination before resolve() is ever
		 * called (see fpm_conf.c). Kept for defensive symmetry. */
		return NULL;
#endif
	}
	if (!strcmp(executor, "async")) {
#ifdef HAVE_FPMNG_ASYNC
		return !strcmp(type->name, "http") ? &fpm_pool_http_async : &fpm_pool_fastcgi_ng_async;
#else
		return NULL;
#endif
	}

	return NULL;
}

int fpm_pool_type_validate_executor(struct fpm_worker_pool_s *wp)
{
	const struct fpm_pool_type_s *type = fpm_pool_type_get(wp->config->type);
	const char *executor = wp->config->executor;

	if (!type || !executor || !*executor) {
		return 0;
	}
	if (strcmp(type->name, "fastcgi-ng") != 0 && strcmp(type->name, "http") != 0) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor is not supported by pool.type = %s",
			wp->config->name, type->name);
		return -1;
	}
	if (strcmp(executor, "classic") != 0 && strcmp(executor, "fiber") != 0 && strcmp(executor, "async") != 0) {
		zlog(ZLOG_ALERT, "[pool %s] unknown pool.executor '%s'; known executors: classic, fiber, async",
			wp->config->name, executor);
		return -1;
	}
#ifndef HAVE_FPMNG_FIBER
	if (!strcmp(executor, "fiber")) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor = fiber: this binary was built without "
			"--enable-fpmng-fiber; rebuild with that flag to use this executor",
			wp->config->name);
		return -1;
	}
#endif
#ifndef HAVE_FPMNG_ASYNC
	if (!strcmp(executor, "async")) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor = async: this binary was built without "
			"--enable-fpmng-async; rebuild with that flag to use this executor",
			wp->config->name);
		return -1;
	}
#endif
	return 0;
}

const struct fpm_pool_type_s *fpm_pool_type_of(struct fpm_worker_pool_s *wp)
{
	const struct fpm_pool_type_s *type = fpm_pool_type_resolve(wp);

	return type ? type : FPM_POOL_TYPE_DEFAULT;
}

int fpm_pool_type_prepare_listening_socket(struct fpm_worker_pool_s *wp)
{
	const struct fpm_pool_type_s *type = fpm_pool_type_of(wp);
	int flags;
	int desired;

	if (!type->requires_listen) {
		return 0;
	}

	/* The socket is an open file description shared by the master and every
	 * child. Set its status once here, while the master owns the configuration,
	 * and repeat this after exec-based reloads to normalize inherited sockets. */
	flags = fcntl(wp->listening_socket, F_GETFL);
	if (flags < 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] failed to read listening socket flags",
			wp->config->name);
		return -1;
	}

	desired = type->listening_socket_nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
	if (desired == flags) {
		return 0;
	}

	if (fcntl(wp->listening_socket, F_SETFL, desired) < 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] failed to set listening socket flags",
			wp->config->name);
		return -1;
	}

	return 0;
}


/* The child must find its pool, and when respawned from the event loop the
 * pointer is lost in fpm_children.c. The scoreboard is per pool and the child
 * receives its own in fpm_scoreboard_init_child(), so matching is enough —
 * without touching fpm_children.c. Called once when the child starts. */
struct fpm_worker_pool_s *fpm_pool_type_current_pool(void)
{
	struct fpm_scoreboard_s *sb = fpm_scoreboard_get();
	struct fpm_worker_pool_s *wp;

	if (!sb) {
		return NULL;
	}

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		if (wp->scoreboard == sb) {
			return wp;
		}
	}

	return NULL;
}
