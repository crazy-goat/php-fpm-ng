/* fpm-ng: pool-type registry. See fpm_pool_type.h. */

#include "fpm_config.h"

#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_type.h"
#include "fpm_http.h"
#include "fpm_http_direct.h"
#include "fpm_http_direct_tls.h"
#include "fpm_http_direct_worker.h"
#include "fpm_http_direct_ops.h"
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

/* Both http-direct executors share it: the certificate is read and the reload
 * machinery armed once per pool in the master, before any child forks, and
 * whichever executor the pool resolves to inherits the result (issue #55). */
static int fpm_pool_type_http_direct_init(struct fpm_worker_pool_s *wp)
{
	return fpm_http_direct_tls_init_main(wp);
}

/* The classic executor only (issue #59): the shared segment behind
 * pm.status_path counts what the children do, so it has to exist before the
 * first of them forks. The worker executor rejects pm.status_path, so it has
 * nothing to allocate. */
static int fpm_pool_type_http_direct_classic_init(struct fpm_worker_pool_s *wp)
{
	if (fpm_pool_type_http_direct_init(wp) < 0) {
		return -1;
	}
	return fpm_http_direct_ops_init_main(wp);
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

/* Effective type/executor combinations. They are not separate pool.type
 * values and therefore do not appear in the type list in messages.
 *
 * Both groups below exist only in a binary built with the corresponding flag
 * (--enable-fpmng-fiber / --enable-fpmng-async, both default "no"). Without
 * the flag the sources are not compiled at all (see build/prepare.sh and
 * sapi/fpmng/config.m4), so these structures and the executor-list entries
 * pointing at them are protected by the same #ifdef. */
#ifdef HAVE_FPMNG_FIBER
static const struct fpm_pool_type_s fpm_pool_fastcgi_ng_fiber = {
	.name                         = "fastcgi-ng",
	.requires_listen              = 1,
	.requires_pm                  = 1,
	.serves_requests              = 1,
	.reuses_request_runtime       = 1,
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
	.reuses_request_runtime       = 1,
	.listening_socket_nonblocking = 1,
	.rejects                      = fpm_coop_rejects,
	.validate                     = fpm_pool_type_fiber_validate,
	.init_main                    = fpm_pool_type_http_concurrent_init,
	.child_main                   = fpm_pool_fiber_child_main,
};
#endif /* HAVE_FPMNG_FIBER */

#ifdef HAVE_FPMNG_ASYNC
static const struct fpm_pool_type_s fpm_pool_fastcgi_ng_async = {
	.name                   = "fastcgi-ng",
	.requires_listen        = 1,
	.requires_pm            = 1,
	.serves_requests        = 1,
	.reuses_request_runtime = 1,
	.rejects                = fpm_pool_async_rejects,
	.validate               = fpm_pool_async_validate,
	.child_main             = fpm_pool_async_child_main,
};

static const struct fpm_pool_type_s fpm_pool_http_async = {
	.name                   = "http",
	.requires_listen        = 1,
	.requires_pm            = 1,
	.serves_requests        = 1,
	.reuses_request_runtime = 1,
	.rejects                = fpm_pool_async_rejects,
	.validate               = fpm_pool_async_validate,
	.init_main              = fpm_pool_type_http_concurrent_init,
	.child_main             = fpm_pool_async_child_main,
};
#endif /* HAVE_FPMNG_ASYNC */

/* POC, task 073: pool.type = http-direct with pool.executor = worker. Same
 * transport, same listener, same master-side bookkeeping; only the CHILD loop
 * is inverted. Classic http-direct runs one script per request from inside an
 * evhttp callback, so a userland event loop's driver would have to call
 * event_base_loop() on a base that is already looping — libevent 2.1.12-stable
 * returns -1 for that and warns "reentrant invocation". Under this executor
 * the worker instead boots ONE script for its lifetime and that script drives
 * the base itself through fpmng_worker_loop(), so Revolt (and therefore amphp)
 * can suspend. See docs/http-direct-revolt-integration.md.
 *
 * An executor rather than a second pool.type for the same reason "fiber" is an
 * executor (fpm_pool_http_fiber above): the transport is unchanged and only
 * the child's execution model differs. .name stays "http-direct" so
 * diagnostics keep naming the type the operator actually configured. */
static const struct fpm_pool_type_s fpm_http_direct_worker = {
	.name                         = "http-direct",
	.requires_listen              = 1,
	.requires_pm                  = 1,
	.serves_requests              = 1,
	.listening_socket_nonblocking = 1,
	.listening_socket_nodelay     = 1,
	.rejects                      = fpm_http_direct_worker_rejects,
	.validate                     = fpm_http_direct_worker_validate,
	/* Same master-side TLS setup as the base type above. An executor variant
	 * replaces the whole type struct rather than overriding fields of it, so
	 * anything the master must do before the first fork has to be repeated
	 * here -- leaving it out made a TLS worker pool fork children that found
	 * no certificate loaded, exit, and be respawned forever (issue #55). */
	.init_main                    = fpm_pool_type_http_direct_init,
	.child_main                   = fpm_http_direct_worker_child_main,
};

/* pool.executor values, as data. "classic" is spelled out here like any other
 * executor so that fpm_pool_type_resolve() looks a name up instead of
 * comparing against one; it resolves to the base type, hence .resolves_to_base.
 *
 * fiber and async each exist only in a binary built with the matching flag
 * (--enable-fpmng-fiber / --enable-fpmng-async, both default "no"): without it
 * the sources are not compiled at all (see build/prepare.sh and
 * sapi/fpmng/config.m4). The entry stays in the list either way, so a
 * configuration asking for one still gets told which flag it needs rather than
 * that the executor does not exist. */
static const struct fpm_pool_executor_s fpm_fastcgi_ng_executors[] = {
	{ .name = "classic", .resolves_to_base = 1 },
	{ .name = "fiber",
#ifdef HAVE_FPMNG_FIBER
	  .type = &fpm_pool_fastcgi_ng_fiber,
#else
	  .build_flag = "--enable-fpmng-fiber",
#endif
	},
	{ .name = "async",
#ifdef HAVE_FPMNG_ASYNC
	  .type = &fpm_pool_fastcgi_ng_async,
#else
	  .build_flag = "--enable-fpmng-async",
#endif
	},
	{ .name = NULL }
};

static const struct fpm_pool_executor_s fpm_http_executors[] = {
	{ .name = "classic", .resolves_to_base = 1 },
	{ .name = "fiber",
#ifdef HAVE_FPMNG_FIBER
	  .type = &fpm_pool_http_fiber,
#else
	  .build_flag = "--enable-fpmng-fiber",
#endif
	},
	{ .name = "async",
#ifdef HAVE_FPMNG_ASYNC
	  .type = &fpm_pool_http_async,
#else
	  .build_flag = "--enable-fpmng-async",
#endif
	},
	{ .name = NULL }
};

/* http-direct ships its own child loop, so it offers its own executor instead
 * of the fiber/async pair — see the comment on fpm_http_direct_worker. */
static const struct fpm_pool_executor_s fpm_http_direct_executors[] = {
	{ .name = "classic", .resolves_to_base = 1 },
	{ .name = "worker", .type = &fpm_http_direct_worker },
	{ .name = NULL }
};

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
		.name                   = "fastcgi-ng",
		.requires_listen        = 1,
		.requires_pm            = 1,
		.serves_requests        = 1,
		.reuses_request_runtime = 1,
		.executors              = fpm_fastcgi_ng_executors,
		.rejects                = fpm_pool_fastcgi_rejects,
	},
	{
		.name                   = "http",
		.requires_listen        = 1,
		.requires_pm            = 1,
		.serves_requests        = 1,
		.reuses_request_runtime = 1,
		.executors              = fpm_http_executors,
		.rejects                = fpm_pool_http_classic_rejects,
		.validate               = fpm_http_validate_pool,
		.init_main              = fpm_pool_type_http_init,
	},
	{
		.name                         = "http-direct",
		.requires_listen              = 1,
		.requires_pm                  = 1,
		.serves_requests              = 1,
		.listening_socket_nonblocking = 1,
		.listening_socket_nodelay     = 1,
		.executors                    = fpm_http_direct_executors,
		.executors_type_specific      = 1,
		.rejects                      = fpm_http_direct_rejects,
		.validate                     = fpm_http_direct_validate,
		.init_main                    = fpm_pool_type_http_direct_classic_init,
		.child_main                   = fpm_http_direct_child_main,
	},
	{
		.name                    = "supervisor",
		.requires_listen         = 0,
		.requires_pm             = 1,	/* pm.* is generated from supervisor.processes; see fpm_pool_supervisor.c */
		.serves_requests         = 0,
		.child_logs_via_master   = 1,	/* the whole policy runs in the child; see fpm_child_log.h */
		.publishes_acme_challenges = 1,	/* see the same flag on "cron" below */
		.rejects                 = fpm_pool_supervisor_rejects,
		.validate                = fpm_pool_supervisor_validate,
		.init_main               = fpm_pool_supervisor_init_main,
		.child_main              = fpm_pool_supervisor_child_main,
		.status                  = fpm_pool_supervisor_status,
	},
	{
		.name                    = "cron",
		.requires_listen         = 0,
		.requires_pm             = 0,	/* validate() always sets pm=static+max_children=1 programmatically */
		.serves_requests         = 0,
		.child_logs_via_master   = 1,	/* same as supervisor: fpm_pool_cron_child_main() is where the policy lives */
		/* docs/NOTES.md section 3l puts the dedicated ACME process in a cron
		 * pool, and "supervisor" above carries the same flag: both are
		 * script-running types that serve no request, which is the property
		 * that matters -- the builtins publish into shared memory and a
		 * publisher must not be a process that also answers requests (issue
		 * #48, criterion 7). Whether the client is scheduled or long-running
		 * is issue #49's decision, and this flag does not prejudge it. */
		.publishes_acme_challenges = 1,
		.rejects                 = fpm_pool_cron_rejects,
		.validate                = fpm_pool_cron_validate,
		.init_main               = fpm_pool_cron_init_main,
		.child_main              = fpm_pool_cron_child_main,
		.status                  = fpm_pool_cron_status,
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

/* Two different questions get answered in the same place at startup and it is
 * worth keeping them apart. fpm_pool_type_check_directives() above asks "does
 * this TYPE support this directive" -- a configuration mistake, identical on
 * every build of this project. This one asks "does this BINARY carry what this
 * type needs" -- the configuration is fine, the executable is not.
 *
 * There is exactly one build where the answer can be no:
 * build/libphp-build.sh links against a distribution's libphp (issue #212) so
 * that `pool.type = fastcgi` and `pool.type = http-direct` can ship as a
 * package with no compilation on the user's side. A distribution libphp is
 * built from unpatched php-src, so patches/0006 -- which lives inside Zend/ --
 * is not in it, and zend_signal_use_persistent_handlers() does not exist
 * there.
 *
 * Keyed off the capability bit, not off a list of type names. A name list
 * would be a second copy of the same fact and would drift the first time a
 * type gains or loses the behaviour; this way a new type that sets
 * reuses_request_runtime is covered on the day it is written, by the person
 * who set the bit.
 *
 * Why refuse instead of degrading: patch 0006 is invisible when it is missing.
 * Such a pool would start, serve traffic and pass its own tests, while the
 * Zend signal handlers were reinstalled on every request -- upstream
 * behaviour under a name that promises the opposite. That makes every
 * measurement taken on it wrong and says nothing while doing it. The fiber and
 * async executors need patches 0007/0008 and are handled differently, by being
 * compiled out entirely (HAVE_FPMNG_FIBER / HAVE_FPMNG_ASYNC): an executor
 * that is not in the type's list is already rejected by name, so there is
 * nothing to add here for them.
 */
int fpm_pool_type_check_build_support(struct fpm_worker_pool_s *wp, const struct fpm_pool_type_s *type)
{
#ifdef HAVE_FPMNG_PERSISTENT_SIGNALS
	(void) wp;
	(void) type;
	return 0;
#else
	if (!type->reuses_request_runtime) {
		return 0;
	}

	zlog(ZLOG_ALERT, "[pool %s] 'pool.type = %s' is not supported by this binary: it was linked "
		"against a distribution libphp, which does not carry patches/0006 (persistent Zend "
		"signal handlers) -- a pool of this type would run with upstream signal behaviour "
		"without saying so", wp->config->name, type->name);
	zlog(ZLOG_ALERT, "[pool %s] use 'pool.type = fastcgi' or 'pool.type = http-direct', which this "
		"binary supports in full, or a build from patched source (build/static-full.sh)",
		wp->config->name);
	return -1;
#endif
}

/* Is this directive one of the type's declared exceptions to its own reject
 * list? Called with the name as it appears in set_directives, which is not
 * NUL-terminated there, hence the explicit length. */
static int fpm_pool_type_directive_excepted(const struct fpm_pool_type_s *type,
	const char *name, size_t len)
{
	const char *const *allow;

	if (!type->reject_exceptions) {
		return 0;
	}
	for (allow = type->reject_exceptions; *allow; allow++) {
		if (strlen(*allow) == len && !strncmp(*allow, name, len)) {
			return 1;
		}
	}
	return 0;
}

int fpm_pool_type_check_directives(struct fpm_worker_pool_s *wp, const struct fpm_pool_type_s *type)
{
	const char *const *reject;
	char where[160];
	int bad = 0;

	if (!type->rejects || !wp->config->set_directives) {
		return 0;
	}
	/* An executor variant carries the plain type name (fpm_http_direct_worker,
	 * fpm_pool_http_fiber), so without this the message would read "not
	 * supported by pool.type = http-direct" for a directive that the SAME type
	 * accepts under pool.executor = classic. Name the combination that is
	 * actually rejecting it. */
	if (wp->config->executor && *wp->config->executor) {
		snprintf(where, sizeof(where), "pool.type = %s with pool.executor = %s",
			type->name, wp->config->executor);
	} else {
		snprintf(where, sizeof(where), "pool.type = %s", type->name);
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
				size_t name_len = end ? (size_t)(end - p - 1) : 0;

				if (!fpm_pool_type_directive_excepted(type, p + 1, name_len)) {
					zlog(ZLOG_ALERT, "[pool %s] '%.*s' is not supported by %s",
						wp->config->name, (int) name_len, p + 1, where);
					bad = 1;
				}
				p = end ? end : p + strlen(p);
			}
		} else if (fpm_conf_directive_was_set(wp->config, *reject)
			&& !fpm_pool_type_directive_excepted(type, *reject, len)) {
			zlog(ZLOG_ALERT, "[pool %s] '%s' is not supported by %s",
				wp->config->name, *reject, where);
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

static const struct fpm_pool_executor_s *fpm_pool_executor_find(
	const struct fpm_pool_type_s *type, const char *name)
{
	const struct fpm_pool_executor_s *e;

	if (!type->executors) {
		return NULL;
	}
	for (e = type->executors; e->name; e++) {
		if (!strcmp(e->name, name)) {
			return e;
		}
	}

	return NULL;
}

/* "classic, fiber, async" for an error message. sep is ", " when the list is
 * read as a set of known names and " or " when it is read as the only
 * acceptable choices; see fpm_pool_type_s.executors_type_specific. */
static void fpm_pool_executor_list(const struct fpm_pool_type_s *type,
	const char *sep, char *buf, size_t len)
{
	const struct fpm_pool_executor_s *e;
	size_t off = 0;

	if (!len) {
		return;
	}
	buf[0] = '\0';

	for (e = type->executors; e && e->name && off + 1 < len; e++) {
		int n = snprintf(buf + off, len - off, "%s%s", off ? sep : "", e->name);

		if (n < 0 || (size_t)n >= len - off) {
			break;
		}
		off += (size_t)n;
	}
}

const struct fpm_pool_type_s *fpm_pool_type_resolve(struct fpm_worker_pool_s *wp)
{
	const struct fpm_pool_type_s *type = fpm_pool_type_get(wp->config->type);
	const struct fpm_pool_executor_s *e;
	const char *executor = wp->config->executor;

	if (!type) {
		return NULL;
	}
	if (!executor || !*executor) {
		return type;
	}

	e = fpm_pool_executor_find(type, executor);
	if (!e) {
		return NULL;
	}
	if (e->resolves_to_base) {
		return type;
	}

	/* e->type == NULL means the executor is not in this build. Unreachable in
	 * practice: fpm_pool_type_validate_executor() already refuses that
	 * combination before resolve() is ever called (see fpm_conf.c). Kept for
	 * defensive symmetry. */
	return e->type;
}

int fpm_pool_type_validate_executor(struct fpm_worker_pool_s *wp)
{
	const struct fpm_pool_type_s *type = fpm_pool_type_get(wp->config->type);
	const struct fpm_pool_executor_s *e;
	const char *executor = wp->config->executor;
	char known[160];

	if (!type || !executor || !*executor) {
		return 0;
	}
	if (!type->executors) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor is not supported by pool.type = %s",
			wp->config->name, type->name);
		return -1;
	}

	e = fpm_pool_executor_find(type, executor);
	if (!e) {
		if (type->executors_type_specific) {
			fpm_pool_executor_list(type, " or ", known, sizeof(known));
			zlog(ZLOG_ALERT, "[pool %s] pool.type = %s supports only pool.executor = %s",
				wp->config->name, type->name, known);
		} else {
			fpm_pool_executor_list(type, ", ", known, sizeof(known));
			zlog(ZLOG_ALERT, "[pool %s] unknown pool.executor '%s'; known executors: %s",
				wp->config->name, executor, known);
		}
		return -1;
	}

	if (!e->resolves_to_base && !e->type) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor = %s: this binary was built without "
			"%s; rebuild with that flag to use this executor",
			wp->config->name, e->name, e->build_flag);
		return -1;
	}

	return 0;
}

const struct fpm_pool_type_s *fpm_pool_type_of(struct fpm_worker_pool_s *wp)
{
	const struct fpm_pool_type_s *type = fpm_pool_type_resolve(wp);

	return type ? type : FPM_POOL_TYPE_DEFAULT;
}

/* TCP_NODELAY for a type that declares it, on the socket the master owns. Why
 * here and not in the child: see listening_socket_nodelay in fpm_pool_type.h.
 *
 * A socket that is not AF_INET/AF_INET6 has no Nagle to turn off, and asking
 * for the option there would fail with ENOPROTOOPT -- so the family decides
 * whether there is anything to do, rather than the error being swallowed. */
static int fpm_pool_type_set_nodelay(struct fpm_worker_pool_s *wp)
{
	struct sockaddr_storage address;
	socklen_t address_len = sizeof(address);
	int on = 1;

	if (getsockname(wp->listening_socket, (struct sockaddr *) &address, &address_len) < 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] failed to read the listening socket's address",
			wp->config->name);
		return -1;
	}
	if (address.ss_family != AF_INET && address.ss_family != AF_INET6) {
		return 0;
	}
	if (setsockopt(wp->listening_socket, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] failed to set TCP_NODELAY on the listening socket",
			wp->config->name);
		return -1;
	}

	return 0;
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

	if (type->listening_socket_nodelay && fpm_pool_type_set_nodelay(wp) < 0) {
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
