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

/* Issue #646: the deadline the soft window or the hard drain ends at:
 * process_control_timeout less the margin, counted from now. The window sets
 * it when the stop starts; the hard drain sets it again when the window ends. */
static void fpm_http_drain_deadline_set(struct fpm_http_gateway_s *gw)
{
	struct timeval now;
	long grace_ms = (long) fpm_global_config.process_control_timeout * 1000 - FPM_HTTP_DRAIN_MARGIN_MS;

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
}

/* Milliseconds from now to the drain deadline, 0 once it has passed. */
static long fpm_http_drain_remaining_ms(const struct fpm_http_gateway_s *gw)
{
	struct timeval now, left;

	fpm_clock_get(&now);
	if (evutil_timercmp(&now, &gw->drain_deadline, >=)) {
		return 0;
	}
	evutil_timersub(&gw->drain_deadline, &now, &left);
	return (long) left.tv_sec * 1000 + (long) left.tv_usec / 1000;
}

/* Issue #646: the soft window is over. The hard drain takes over with a deadline
 * of its own (fpm_http_drain_start() sets it). The timer is not persistent, so
 * it may be freed from its own callback. */
static void fpm_http_drain_soft_timeout(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_http_gateway_s *gw = arg;

	(void) fd;
	(void) what;
	fpm_http_drain_start(gw);
}

void fpm_http_drain_soft_start(struct fpm_http_gateway_s *gw)
{
	struct timeval wait;
	long left_ms;

	if (gw->stopping || gw->soft_draining) {
		return; /* a second SIGUSR1 must not restart the window */
	}
	gw->soft_draining = 1;
	fpm_http_drain_deadline_set(gw);
	left_ms = fpm_http_drain_remaining_ms(gw);

	zlog(ZLOG_NOTICE, "[pool %s] http gateway (pid %d) is stopping: the readiness probe answers 503 "
					  "and new connections are still served for up to %ld ms",
			gw->pool, (int) getpid(), left_ms);

	/* The timer ends the window. If it cannot be armed, the hard drain starts
	 * now: a gateway that cannot time its own window must not keep serving
	 * past the deadline the master enforces. */
	wait.tv_sec = left_ms / 1000;
	wait.tv_usec = (left_ms % 1000) * 1000;
	gw->soft_timer = evtimer_new(gw->base, fpm_http_drain_soft_timeout, gw);
	if (!gw->soft_timer || event_add(gw->soft_timer, &wait) != 0) {
		if (gw->soft_timer) {
			event_free(gw->soft_timer);
			gw->soft_timer = NULL;
		}
		fpm_http_drain_start(gw);
	}
}

void fpm_http_drain_start(struct fpm_http_gateway_s *gw)
{
	struct timeval interval = { 0, FPM_HTTP_DRAIN_TICK_MS * 1000 };

	if (gw->stopping) {
		return; /* a second SIGQUIT must not restart the deadline */
	}
	gw->stopping = 1;
	if (gw->soft_timer) {
		event_free(gw->soft_timer);
		gw->soft_timer = NULL;
	}
	/* Issue #646: the hard drain gets its own deadline, counted from this call.
	 * After a soft window that is the end of the window. Reusing the window's
	 * deadline left 0 ms for the in-flight requests (review of #646). */
	fpm_http_drain_deadline_set(gw);

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

	zlog(ZLOG_NOTICE, "[pool %s] http gateway (pid %d) is draining: no new connections, "
					  "finishing in-flight requests for up to %ld ms",
			gw->pool, (int) getpid(), fpm_http_drain_remaining_ms(gw));

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
