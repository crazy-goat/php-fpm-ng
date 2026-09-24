/* fpm-ng: TLS termination for pool.type = http-direct -- the half that needs
 * OpenSSL (issue #55). Compiled only when the build was configured with
 * --enable-fpmng-tls (issue #280); fpm_http_direct_tls.c holds the half that
 * is in every build and the no-op stubs that stand in for this file when the
 * flag was not given. See fpm_http_direct_tls.h for the contract both halves
 * implement, and build/prepare.sh for why the file is named fpm_tls_* rather
 * than fpm_http_direct_tls_openssl.c: the TLS source group is matched by that
 * name prefix, so a file joins it by being named for it.
 */
#include "fpm_config.h"

#include <stdlib.h>
#include <string.h>

#include <event2/event.h>
#include <event2/http.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_http_direct_tls.h"
#include "fpm_tls_http.h"
#include "fpm_tls_reload.h"
#include "zlog.h"

/* One entry per TLS-configured direct pool. Allocated in the master before
 * the first fork, so every child inherits the whole list -- including the
 * PEM bytes and the reload machinery's shared-memory handle -- and a child
 * finds its own entry by pool name. The list is never freed: FPM's master
 * keeps its pools for the life of the process, and a child exit()s rather
 * than unwinds, exactly as struct fpm_http_gateway_s does. */
struct fpm_http_direct_tls_s {
	char *pool;
	struct fpm_tls_http_s *tls;
	struct fpm_tls_reload_s *reload;
	/* Child-only. Never inherited through fork(): an SSL_CTX carries per
	 * process state (and, after a reload, a different certificate in each
	 * process), so it is built in fpm_http_direct_tls_child_attach() and is
	 * NULL everywhere else -- same rule as fpm_tls_http.h states for the
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
	struct bufferevent *bev = fpm_tls_http_bevcb(base, st->ctx);

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
		1 / (FPM_HTTP_DIRECT_TLS_WRITE_IDLE == FPM_TLS_HTTP_WRITE_IDLE) };
	(void) idle_values_match;
	return fpm_tls_http_write_output(bev, poll_events);
}

void fpm_http_direct_tls_notify_written(struct bufferevent *bev)
{
	fpm_tls_http_notify_written(bev);
}

int fpm_http_direct_tls_shutdown_step(struct bufferevent *bev, short *poll_events)
{
	/* The two public headers meet here, as they already do for the write-step's
	 * shared idle value. Assert the result mapping stays mechanical. */
	enum { shutdown_values_match =
		1 / (FPM_HTTP_DIRECT_TLS_SHUTDOWN_NOT_APPLICABLE == FPM_TLS_HTTP_SHUTDOWN_NOT_APPLICABLE
			&& FPM_HTTP_DIRECT_TLS_SHUTDOWN_PENDING == FPM_TLS_HTTP_SHUTDOWN_PENDING
			&& FPM_HTTP_DIRECT_TLS_SHUTDOWN_SENT == FPM_TLS_HTTP_SHUTDOWN_SENT
			&& FPM_HTTP_DIRECT_TLS_SHUTDOWN_FAILED == FPM_TLS_HTTP_SHUTDOWN_FAILED) };
	int result = fpm_tls_http_shutdown_step(bev, poll_events);

	(void) shutdown_values_match;
	return result;
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
	return fpm_tls_http_validate(c->name, c->http_tls_cert, c->http_tls_key,
		c->http_tls_min_version, c->http_tls_sni_cert,
		c->http_tls_verify_client, c->http_tls_client_ca);
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
	st->tls = fpm_tls_http_load(c->name, c->http_tls_cert, c->http_tls_key,
		c->http_tls_min_version, c->http_tls_sni_cert,
		c->http_tls_verify_client, c->http_tls_client_ca);
	if (!st->pool || !st->tls) {
		/* fpm_tls_http_load() already said what went wrong. Refusing to start
		 * is the point: validation passed a moment ago, so a failure here is
		 * the file changing underneath us, and starting anyway would mean
		 * accepting on a port configured as HTTPS with no certificate. */
		fpm_tls_http_free(st->tls);
		free(st->pool);
		free(st);
		return -1;
	}
	/* http.tls_reload_check: unset means the default, an explicit 0 means off.
	 * Same distinction, and the same default, as the gateway -- an operator
	 * who renews certificates the same way for both pool types should not have
	 * to configure them differently. */
	interval = fpm_conf_directive_was_set(c, "http.tls_reload_check")
		? c->http_tls_reload_check : FPM_TLS_RELOAD_CHECK_DEFAULT;
	st->reload = fpm_tls_reload_master_init(c->name, c->http_tls_cert, c->http_tls_key,
		c->http_tls_min_version, st->tls, interval);
	/* A NULL reload is not fatal: the pool serves the certificate it loaded,
	 * it just cannot pick up a new one without a restart, and
	 * fpm_tls_reload_master_init() logged why. */
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
	st->ctx = fpm_tls_reload_child_ctx_new(st->reload);
	if (!st->ctx) {
		st->ctx = fpm_tls_http_ctx_new(wp->config->name, st->tls);
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
	fpm_tls_reload_child_init(st->reload, base, http, &st->ctx,
		fpm_http_direct_tls_bevcb, st);
	evhttp_set_bevcb(http, fpm_http_direct_tls_bevcb, st);
	return 0;
}
