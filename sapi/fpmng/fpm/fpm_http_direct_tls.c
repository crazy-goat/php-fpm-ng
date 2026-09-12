/* fpm-ng: TLS termination for pool.type = http-direct (issue #55) -- the half
 * that is in EVERY build, because it needs no OpenSSL: it only reads the
 * configuration. The half that terminates TLS lives in fpm_tls_http_direct.c
 * and is compiled only with --enable-fpmng-tls (issue #280); the stubs at the
 * bottom of this file stand in for it otherwise.
 * See fpm_http_direct_tls.h for what this file deliberately is not.
 */
#include "fpm_config.h"

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_http_direct_tls.h"
#include "zlog.h"

bool fpm_http_direct_tls_enabled(struct fpm_worker_pool_s *wp)
{
	return wp->config->http_tls_cert && *wp->config->http_tls_cert;
}

/* Kept out of the two fpm_http_direct_tls_validate() bodies -- the one in
 * fpm_tls_http_direct.c and the stub below -- so that an operator gets the
 * message about what they actually configured wrong, rather than "this build
 * has no TLS" for a pool that never asked for a certificate. */
int fpm_http_direct_tls_validate_pairing(struct fpm_worker_pool_s *wp)
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

#ifndef HAVE_FPM_HTTP_TLS

/* No-op stand-ins for fpm_tls_http_direct.c, which the build leaves out unless
 * it was configured with --enable-fpmng-tls (issue #280). They exist so that
 * fpm_http_direct.c, fpm_http_direct_worker.c, fpm_http_direct_request.c and
 * fpm_pool_type.c call the same functions in both builds and carry no #ifdef
 * of their own. Nothing here can be reached with TLS actually in use: a pool
 * with http.tls_cert is refused below, before anything forks. */

int fpm_http_direct_tls_validate(struct fpm_worker_pool_s *wp)
{
	if (fpm_http_direct_tls_validate_pairing(wp) < 0) {
		return -1;
	}
	if (fpm_http_direct_tls_enabled(wp)) {
		/* Refused, not downgraded. An operator who configured a certificate
		 * asked for HTTPS on that port; serving plain HTTP there instead is
		 * the one outcome that must not happen quietly. */
		zlog(ZLOG_ALERT, "[pool %s] http.tls_cert requires php-fpm-ng to be built with TLS support: "
			"rebuild with ./configure --enable-fpmng-tls (needs libevent_openssl and OpenSSL)",
			wp->config->name);
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
