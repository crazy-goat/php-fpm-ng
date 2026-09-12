/* fpm-ng: TLS termination for pool.type = http-direct (issue #55).
 * See fpm_http_direct_tls.h for what this file deliberately is not.
 */
#include "fpm_config.h"

#include <stdlib.h>
#include <string.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_http_direct_tls.h"
#include "zlog.h"

bool fpm_http_direct_tls_enabled(struct fpm_worker_pool_s *wp)
{
	return wp->config->http_tls_cert && *wp->config->http_tls_cert;
}

/* The half of validation that is the same in every build, because none of it
 * needs OpenSSL: it only reads the configuration. Kept out of the two
 * fpm_http_direct_tls_validate() bodies below so that an operator gets the
 * message about what they actually configured wrong, rather than "this build
 * has no TLS" for a pool that never asked for a certificate. */
static int fpm_http_direct_tls_validate_pairing(struct fpm_worker_pool_s *wp)
{
	struct fpm_worker_pool_config_s *c = wp->config;

	if (fpm_http_direct_tls_enabled(wp)) {
		if (!c->http_tls_key || !*c->http_tls_key) {
			zlog(ZLOG_ALERT, "[pool %s] http.tls_cert requires http.tls_key", c->name);
			return -1;
		}
		return 0;
	}
	if (c->http_tls_key && *c->http_tls_key) {
		zlog(ZLOG_ALERT, "[pool %s] http.tls_key without http.tls_cert has nothing to attach the key to",
			c->name);
		return -1;
	}
	/* The two knobs that only mean anything once a certificate is served.
	 * Refused rather than ignored, so a typo in http.tls_cert cannot leave a
	 * pool serving plain HTTP while its configuration still reads as
	 * TLS-tuned. */
	if ((c->http_tls_sni_cert && *c->http_tls_sni_cert) || c->http_tls_min_version) {
		zlog(ZLOG_ALERT, "[pool %s] http.tls_min_version and http.tls_sni_cert require http.tls_cert",
			c->name);
		return -1;
	}
	return 0;
}

#ifdef HAVE_FPM_HTTP_TLS

#include <event2/event.h>
#include <event2/http.h>

#include "fpm_http_tls.h"
#include "fpm_http_tls_reload.h"

/* One entry per TLS-configured direct pool. Allocated in the master before
 * the first fork, so every child inherits the whole list -- including the
 * PEM bytes and the reload machinery's shared-memory handle -- and a child
 * finds its own entry by pool name. The list is never freed: FPM's master
 * keeps its pools for the life of the process, and a child exit()s rather
 * than unwinds, exactly as struct fpm_http_gateway_s does. */
struct fpm_http_direct_tls_s {
	char *pool;
	struct fpm_http_tls_s *tls;
	struct fpm_http_tls_reload_s *reload;
	/* Child-only. Never inherited through fork(): an SSL_CTX carries per
	 * process state (and, after a reload, a different certificate in each
	 * process), so it is built in fpm_http_direct_tls_child_attach() and is
	 * NULL everywhere else -- same rule as fpm_http_tls.h states for the
	 * gateway. */
	SSL_CTX *ctx;
	/* Child-only, like ctx: the caller's accept gate, see the header. */
	void (*on_accept)(void *, struct bufferevent *);
	void *on_accept_arg;
	struct fpm_http_direct_tls_s *next;
};

static struct fpm_http_direct_tls_s *fpm_http_direct_tls_pools = NULL;

static struct fpm_http_direct_tls_s *fpm_http_direct_tls_find(const char *pool)
{
	struct fpm_http_direct_tls_s *st;

	for (st = fpm_http_direct_tls_pools; st; st = st->next) {
		if (!strcmp(st->pool, pool)) {
			return st;
		}
	}
	return NULL;
}

/* evhttp_set_bevcb() callback. `arg` is the pool's state, not the SSL_CTX, so
 * that a certificate reload -- which reinstalls this same pair and writes the
 * new context through st->ctx -- is picked up by the next connection without
 * a second registration path. */
static struct bufferevent *fpm_http_direct_tls_bevcb(struct event_base *base, void *arg)
{
	struct fpm_http_direct_tls_s *st = arg;
	/* After the bufferevent, not before: the hook is given it, and on this
	 * path it is an SSL bufferevent whose descriptor evhttp attaches next --
	 * the same point in the connection's life as on a plain pool. */
	struct bufferevent *bev = fpm_http_tls_bevcb(base, st->ctx);

	if (st->on_accept) {
		st->on_accept(st->on_accept_arg, bev);
	}
	return bev;
}

ev_ssize_t fpm_http_direct_tls_write(struct bufferevent *bev, short *poll_events)
{
	/* The one place both headers are in scope, so the one place the two names
	 * for "nothing to write" can be checked against each other. */
	enum { idle_values_match =
		1 / (FPM_HTTP_DIRECT_TLS_WRITE_IDLE == FPM_HTTP_TLS_WRITE_IDLE) };
	(void) idle_values_match;
	return fpm_http_tls_write_output(bev, poll_events);
}

void fpm_http_direct_tls_notify_written(struct bufferevent *bev)
{
	fpm_http_tls_notify_written(bev);
}

int fpm_http_direct_tls_validate(struct fpm_worker_pool_s *wp)
{
	struct fpm_worker_pool_config_s *c = wp->config;

	if (fpm_http_direct_tls_validate_pairing(wp) < 0) {
		return -1;
	}
	if (!fpm_http_direct_tls_enabled(wp)) {
		return 0;
	}
	/* Same check the `http` pool type runs, for the same reason: a bad path
	 * or a key that does not match its certificate must fail here, before the
	 * first child forks, rather than as a crash or a silent plain-HTTP
	 * fallback on an accepted connection. */
	return fpm_http_tls_validate(c->name, c->http_tls_cert, c->http_tls_key,
		c->http_tls_min_version, c->http_tls_sni_cert);
}

int fpm_http_direct_tls_init_main(struct fpm_worker_pool_s *wp)
{
	struct fpm_worker_pool_config_s *c = wp->config;
	struct fpm_http_direct_tls_s *st;
	int interval;

	if (!fpm_http_direct_tls_enabled(wp) || fpm_http_direct_tls_find(c->name)) {
		return 0;
	}
	st = calloc(1, sizeof(*st));
	if (!st) {
		zlog(ZLOG_SYSERROR, "[pool %s] http-direct: cannot allocate TLS state", c->name);
		return -1;
	}
	st->pool = strdup(c->name);
	st->tls = fpm_http_tls_load(c->name, c->http_tls_cert, c->http_tls_key,
		c->http_tls_min_version, c->http_tls_sni_cert);
	if (!st->pool || !st->tls) {
		/* fpm_http_tls_load() already said what went wrong. Refusing to start
		 * is the point: validation passed a moment ago, so a failure here is
		 * the file changing underneath us, and starting anyway would mean
		 * accepting on a port configured as HTTPS with no certificate. */
		fpm_http_tls_free(st->tls);
		free(st->pool);
		free(st);
		return -1;
	}
	/* http.tls_reload_check: unset means the default, an explicit 0 means off.
	 * Same distinction, and the same default, as the gateway -- an operator
	 * who renews certificates the same way for both pool types should not have
	 * to configure them differently. */
	interval = fpm_conf_directive_was_set(c, "http.tls_reload_check")
		? c->http_tls_reload_check : FPM_HTTP_TLS_RELOAD_CHECK_DEFAULT;
	st->reload = fpm_http_tls_reload_master_init(c->name, c->http_tls_cert, c->http_tls_key,
		c->http_tls_min_version, st->tls, interval);
	/* A NULL reload is not fatal: the pool serves the certificate it loaded,
	 * it just cannot pick up a new one without a restart, and
	 * fpm_http_tls_reload_master_init() logged why. */
	st->next = fpm_http_direct_tls_pools;
	fpm_http_direct_tls_pools = st;
	return 0;
}

int fpm_http_direct_tls_child_attach(struct fpm_worker_pool_s *wp, struct event_base *base,
	struct evhttp *http, void (*on_accept)(void *, struct bufferevent *), void *on_accept_arg)
{
	struct fpm_http_direct_tls_s *st;

	if (!fpm_http_direct_tls_enabled(wp)) {
		return 0;
	}
	st = fpm_http_direct_tls_find(wp->config->name);
	if (!st) {
		zlog(ZLOG_ERROR, "[pool %s] http-direct: TLS is configured but no certificate was loaded before fork",
			wp->config->name);
		return -1;
	}
	/* From the currently published generation, not from st->tls: st->tls is
	 * what the master read once before the FIRST fork, so a child respawned
	 * after N reloads would otherwise start on -- and, believing itself up to
	 * date, keep -- the startup certificate. That is issue #91, fixed here the
	 * same way it was fixed for the gateway. At startup the published slot IS
	 * generation 0, i.e. st->tls's bytes, so nothing about startup changes. */
	st->ctx = fpm_http_tls_reload_child_ctx_new(st->reload);
	if (!st->ctx) {
		st->ctx = fpm_http_tls_ctx_new(wp->config->name, st->tls);
	}
	if (!st->ctx) {
		return -1;
	}
	/* Before the pair is registered anywhere, so neither the reload path nor
	 * the first connection can run the bevcb without it. */
	st->on_accept = on_accept;
	st->on_accept_arg = on_accept_arg;
	/* Before evhttp_set_bevcb() below only by convention -- the reload code
	 * records the pair and first uses it on a later generation change. A no-op
	 * when reload is NULL or its interval is 0. */
	fpm_http_tls_reload_child_init(st->reload, base, http, &st->ctx,
		fpm_http_direct_tls_bevcb, st);
	evhttp_set_bevcb(http, fpm_http_direct_tls_bevcb, st);
	return 0;
}

#else /* !HAVE_FPM_HTTP_TLS */

int fpm_http_direct_tls_validate(struct fpm_worker_pool_s *wp)
{
	if (fpm_http_direct_tls_validate_pairing(wp) < 0) {
		return -1;
	}
	if (fpm_http_direct_tls_enabled(wp)) {
		/* Refused, not downgraded. An operator who configured a certificate
		 * asked for HTTPS on that port; serving plain HTTP there instead is
		 * the one outcome that must not happen quietly. */
		zlog(ZLOG_ALERT, "[pool %s] http.tls_cert requires php-fpm-ng to be built with TLS support "
			"(libevent_openssl and/or OpenSSL were not found at build time)", wp->config->name);
		return -1;
	}
	return 0;
}

ev_ssize_t fpm_http_direct_tls_write(struct bufferevent *bev, short *poll_events)
{
	(void) bev;
	*poll_events = 0;
	return -1;
}

void fpm_http_direct_tls_notify_written(struct bufferevent *bev)
{
	(void) bev;
}

int fpm_http_direct_tls_init_main(struct fpm_worker_pool_s *wp)
{
	(void) wp;
	return 0;
}

int fpm_http_direct_tls_child_attach(struct fpm_worker_pool_s *wp, struct event_base *base,
	struct evhttp *http, void (*on_accept)(void *, struct bufferevent *), void *on_accept_arg)
{
	(void) wp; (void) base; (void) http; (void) on_accept; (void) on_accept_arg;
	return 0;
}

#endif
