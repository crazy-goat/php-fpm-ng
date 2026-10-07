/* fpm-ng: the gateway's graceful drain (issue #641).
 *
 * On stop and on reload the master sends SIGQUIT to every gateway process
 * instead of the SIGTERM that used to cut in-flight requests. This is what the
 * gateway does with it: stop accepting, close the connections that are idle
 * between keep-alive requests, let in-flight requests finish up to the hard
 * deadline the master also enforces (process_control_timeout), then leave the
 * event loop and exit. The master SIGKILLs whatever is still alive at the
 * deadline, so a wedged process is bounded even if this code never runs.
 *
 * The deadline is process_control_timeout and nothing else -- there is no new
 * directive. With the stock value of 0 the gateway is killed at once, exactly
 * as the SIGTERM it replaces did; an operator who wants a graceful stop sets
 * process_control_timeout, as docs/shutdown-timeouts.md already recommends.
 */
#ifndef FPM_HTTP_DRAIN_H
#define FPM_HTTP_DRAIN_H 1

#include "fpm_config.h"

#ifdef HAVE_FPM_HTTP

struct fpm_http_gateway_s;

/* Begins the drain in this gateway process. Idempotent: a second SIGQUIT is a
 * no-op, so a retrying master does not shorten the deadline. Must be called
 * from the process's own event loop (it is an evsignal callback), never from
 * signal context. */
void fpm_http_drain_start(struct fpm_http_gateway_s *gw);

/* How often the drain tick checks for finished work and for the deadline. A
 * drained gateway exits within one tick of its last response, so this also
 * bounds how long a clean drain lingers. */
#define FPM_HTTP_DRAIN_TICK_MS 20

/* The gateway exits this long before the master's own SIGKILL deadline, so the
 * ordinary case is a clean exit rather than a kill: both deadlines start from
 * process_control_timeout, but the gateway learns of the SIGQUIT a moment
 * after the master sent it. */
#define FPM_HTTP_DRAIN_MARGIN_MS 100

#endif /* HAVE_FPM_HTTP */
#endif /* FPM_HTTP_DRAIN_H */
