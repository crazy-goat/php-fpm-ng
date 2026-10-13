/* See fpm_http_accept_backoff.h for the failure this prevents. */

#include "fpm_config.h"

#include "fpm.h"
#include "fpm_http_accept_backoff.h"

#ifdef HAVE_FPM_HTTP

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <event2/listener.h>
#include <event2/util.h>

#include "zlog.h"

/* One line per this many seconds while the condition lasts: polling every
 * 100 ms would otherwise write ten lines a second. */
#define FPM_HTTP_ACCEPT_BACKOFF_LOG_SEC 10

struct fpm_http_accept_backoff {
	struct evconnlistener *listener;
	struct event *timer;
	const char *pool;
	const char *what;
	/* Asked when the pause ends: false keeps the listener closed because the
	 * owner's own gate (max_connections, accept_threshold cooldown, a request
	 * in progress) wants it closed. NULL means always resume. */
	int (*may_resume)(void *arg);
	void *arg;
	int paused;
	time_t last_log;
	unsigned long suppressed;
};

/* libevent hands the error callback only the listener: the user pointer of an
 * evhttp listener is evhttp's own (evconnlistener_set_cb() would replace it
 * and break accepting). A gateway process has at most the TLS listener and
 * http.plain_listen; an http-direct child has exactly one listener. So a
 * fixed table keyed by listener is enough. */
#define FPM_HTTP_ACCEPT_BACKOFF_MAX 4
static struct fpm_http_accept_backoff *fpm_http_accept_backoff_table[FPM_HTTP_ACCEPT_BACKOFF_MAX];

static struct fpm_http_accept_backoff *fpm_http_accept_backoff_find(struct evconnlistener *listener) /* {{{ */
{
	for (int i = 0; i < FPM_HTTP_ACCEPT_BACKOFF_MAX; i++) {
		struct fpm_http_accept_backoff *b = fpm_http_accept_backoff_table[i];

		if (b && b->listener == listener) {
			return b;
		}
	}
	return NULL;
}
/* }}} */

static void fpm_http_accept_backoff_resume(evutil_socket_t fd, short what, void *arg) /* {{{ */
{
	struct fpm_http_accept_backoff *b = arg;

	(void) fd;
	(void) what;
	b->paused = 0;
	if (!b->may_resume || b->may_resume(b->arg)) {
		evconnlistener_enable(b->listener);
	}
}
/* }}} */

static void fpm_http_accept_backoff_error(struct evconnlistener *listener, void *arg) /* {{{ */
{
	struct fpm_http_accept_backoff *b = fpm_http_accept_backoff_find(listener);
	struct timeval tv = { 0, FPM_HTTP_ACCEPT_BACKOFF_MS * 1000 };
	int err = EVUTIL_SOCKET_ERROR();
	time_t now = time(NULL);

	(void) arg;
	if (!b) {
		return;
	}

	/* Pausing is right for any accept() failure that is not retriable by
	 * itself, not only descriptor exhaustion: ENOBUFS and ENOMEM spin the
	 * same way and clear the same way. */
	evconnlistener_disable(listener);
	if (b->timer && event_add(b->timer, &tv) == 0) {
		b->paused = 1;
	} else {
		/* No timer to resume with: leaving the listener paused forever is
		 * worse than spinning, so put it back. */
		evconnlistener_enable(listener);
	}

	if (b->last_log == 0 || now - b->last_log >= FPM_HTTP_ACCEPT_BACKOFF_LOG_SEC) {
		zlog(ZLOG_WARNING, "[pool %s] http: accept() on the %s listener failed: %s; pausing accept for %d ms and retrying (%lu similar failures not logged)",
				b->pool, b->what, evutil_socket_error_to_string(err), FPM_HTTP_ACCEPT_BACKOFF_MS, b->suppressed);
		b->last_log = now;
		b->suppressed = 0;
	} else {
		b->suppressed++;
	}
}
/* }}} */

int fpm_http_accept_backoff_install(struct event_base *base, struct evhttp_bound_socket *bound,
		const char *pool, const char *what, int (*may_resume)(void *arg), void *arg) /* {{{ */
{
	struct fpm_http_accept_backoff *b;
	int slot;
	struct evconnlistener *listener = bound ? evhttp_bound_socket_get_listener(bound) : NULL;

	if (!listener) {
		return -1;
	}
	b = calloc(1, sizeof(*b));
	if (!b) {
		return -1;
	}
	for (slot = 0; slot < FPM_HTTP_ACCEPT_BACKOFF_MAX; slot++) {
		if (!fpm_http_accept_backoff_table[slot]) {
			break;
		}
	}
	if (slot == FPM_HTTP_ACCEPT_BACKOFF_MAX) {
		free(b);
		return -1;
	}
	b->listener = listener;
	b->pool = pool;
	b->what = what;
	b->may_resume = may_resume;
	b->arg = arg;
	/* Lives as long as the listener: an owner that deletes the listener
	 * (http-direct retiring or stopping) must call
	 * fpm_http_accept_backoff_remove() first. */
	b->timer = evtimer_new(base, fpm_http_accept_backoff_resume, b);
	if (!b->timer) {
		free(b);
		return -1;
	}
	fpm_http_accept_backoff_table[slot] = b;
	evconnlistener_set_error_cb(listener, fpm_http_accept_backoff_error);
	return 0;
}
/* }}} */

int fpm_http_accept_backoff_paused(struct evhttp_bound_socket *bound) /* {{{ */
{
	struct fpm_http_accept_backoff *b = bound ? fpm_http_accept_backoff_find(evhttp_bound_socket_get_listener(bound)) : NULL;

	return b && b->paused;
}
/* }}} */

void fpm_http_accept_backoff_remove(struct evhttp_bound_socket *bound) /* {{{ */
{
	struct evconnlistener *listener = bound ? evhttp_bound_socket_get_listener(bound) : NULL;

	for (int i = 0; listener && i < FPM_HTTP_ACCEPT_BACKOFF_MAX; i++) {
		struct fpm_http_accept_backoff *b = fpm_http_accept_backoff_table[i];

		if (b && b->listener == listener) {
			fpm_http_accept_backoff_table[i] = NULL;
			event_free(b->timer);
			free(b);
			return;
		}
	}
}
/* }}} */

#endif /* HAVE_FPM_HTTP */
