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
	time_t last_log;
	unsigned long suppressed;
};

/* libevent hands the error callback only the listener: the user pointer of an
 * evhttp listener is evhttp's own (evconnlistener_set_cb() would replace it
 * and break accepting). A gateway process has at most the TLS listener and
 * http.plain_listen, so a fixed table keyed by listener is enough. */
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
	evconnlistener_enable(b->listener);
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
	if (b->timer) {
		(void) event_add(b->timer, &tv);
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
		const char *pool, const char *what) /* {{{ */
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
	/* Lives as long as the process: the listener does. */
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

#endif /* HAVE_FPM_HTTP */
