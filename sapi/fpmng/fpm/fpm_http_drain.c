/* See fpm_http_drain.h for what the drain is and why it exists (issue #641). */

#include "fpm_config.h"

#include "fpm.h"
#include "fpm_http.h"

#ifdef HAVE_FPM_HTTP

#include <signal.h>
#include <unistd.h>

#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/util.h>

#include "fpm_conf.h"
#include "fpm_clock.h"
#include "fpm_http_accept_backoff.h"
#include "zlog.h"

#include "fpm_http_internal.h"
#include "fpm_http_drain.h"

/* The drain tick: close what is idle, leave when nothing is left or the
 * deadline is up. Runs in the event loop, so fpm_http_gateway_drain_step() may
 * touch evhttp. */
static void fpm_http_drain_tick(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_http_gateway_s *gw = arg;
	struct timeval now;
	unsigned busy;

	(void) fd;
	(void) what;

	busy = fpm_http_gateway_drain_step(gw);
	fpm_clock_get(&now);

	if (busy == 0) {
		zlog(ZLOG_NOTICE, "[pool %s] http gateway (pid %d) drained and is exiting",
				gw->pool, (int) getpid());
		event_base_loopbreak(gw->base);
		return;
	}
	if (evutil_timercmp(&now, &gw->drain_deadline, >=)) {
		zlog(ZLOG_NOTICE, "[pool %s] http gateway (pid %d) stopped waiting for %u connection(s) "
						  "still in flight after process_control_timeout and is exiting",
				gw->pool, (int) getpid(), busy);
		event_base_loopbreak(gw->base);
		return;
	}
}

void fpm_http_drain_start(struct fpm_http_gateway_s *gw)
{
	struct timeval interval = { 0, FPM_HTTP_DRAIN_TICK_MS * 1000 };
	struct timeval now;
	long grace_ms;
	int pct = fpm_global_config.process_control_timeout;

	if (gw->stopping) {
		return; /* a second SIGQUIT must not restart the deadline */
	}
	gw->stopping = 1;

	/* Out of accept first: the listening socket belongs to the whole pool, so
	 * every connection this process does not take is one the master is about
	 * to stop anyway. fpm_http_accept_backoff_remove() must run before
	 * evhttp_del_accept_socket(), which frees the listener the backoff timer
	 * would otherwise re-enable (issue #729). */
	if (gw->tls_bound) {
		fpm_http_accept_backoff_remove(gw->tls_bound);
		evhttp_del_accept_socket(gw->http, gw->tls_bound);
		gw->tls_bound = NULL;
	}
	if (gw->plain_bound) {
		fpm_http_accept_backoff_remove(gw->plain_bound);
		evhttp_del_accept_socket(gw->plain_http, gw->plain_bound);
		gw->plain_bound = NULL;
	}

	grace_ms = (long) pct * 1000 - FPM_HTTP_DRAIN_MARGIN_MS;
	if (grace_ms < 0) {
		grace_ms = 0;
	}
	fpm_clock_get(&now);
	gw->drain_deadline = now;
	gw->drain_deadline.tv_sec += grace_ms / 1000;
	gw->drain_deadline.tv_usec += (grace_ms % 1000) * 1000;
	if (gw->drain_deadline.tv_usec >= 1000000) {
		gw->drain_deadline.tv_sec++;
		gw->drain_deadline.tv_usec -= 1000000;
	}

	zlog(ZLOG_NOTICE, "[pool %s] http gateway (pid %d) is draining: no new connections, "
					  "finishing in-flight requests for up to %ld ms",
			gw->pool, (int) getpid(), grace_ms);

	gw->drain_tick = event_new(gw->base, -1, EV_PERSIST, fpm_http_drain_tick, gw);
	if (!gw->drain_tick) {
		/* No way to poll for finished work. Leaving now rather than hanging is
		 * the only safe answer; the master's SIGKILL bounds this process
		 * either way. */
		event_base_loopbreak(gw->base);
		return;
	}
	if (event_add(gw->drain_tick, &interval) != 0) {
		event_free(gw->drain_tick);
		gw->drain_tick = NULL;
		event_base_loopbreak(gw->base);
	}
}

#endif /* HAVE_FPM_HTTP */
