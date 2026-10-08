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
 * directive. With the stock value of 0 there is no window and no hold: the
 * gateway is killed at once, exactly as the SIGTERM it replaces did. An
 * operator who wants a graceful stop sets process_control_timeout, as
 * docs/shutdown-timeouts.md already recommends.
 *
 * Issue #646: a gateway with http.ready_path first runs a soft drain, which
 * lasts the same window and keeps it serving, so a probe sees 503 "draining"
 * and a new connection is answered, not refused (fpm_http_drain_soft_start()).
 * The window still ends at the same deadline. The children of the pools that
 * such a gateway routes to are held back for the same window
 * (fpm_http_pool_held_for_window()), so the application still answers while
 * the gateway does; they get their signal when the last such gateway exits
 * (fpm_pctl_release_deferred()).
 *
 * What "in flight" does NOT cover (phase-1 limitation, issue #641 review): a
 * request whose body is still being uploaded. evhttp buffers a whole body
 * before dispatching it. Without a ready_path window the master stops the
 * target pool's workers before it drains the gateways, so no worker is left
 * when the upload dispatches. Draining an upload needs the gateway drained
 * before the workers are stopped, which this phase does not do.
 */
#ifndef FPM_HTTP_DRAIN_H
#define FPM_HTTP_DRAIN_H 1

#include "fpm_config.h"

#ifdef HAVE_FPM_HTTP

struct fpm_http_gateway_s;
struct fpm_worker_pool_s;

/* Begins the drain in this gateway process. Idempotent: a second SIGQUIT is a
 * no-op, so a retrying master does not shorten the deadline. Must be called
 * from the process's own event loop (it is an evsignal callback), never from
 * signal context. The deadline is taken from the soft drain when one ran
 * (fpm_http_drain_soft_start), otherwise it starts now. */
void fpm_http_drain_start(struct fpm_http_gateway_s *gw);

/* Issue #646: the soft drain, started by SIGUSR1 from the master at the start
 * of a stop or reload, before the children are signalled. This process keeps
 * its listeners and serves, and the readiness probe answers 503 "draining".
 * The window ends at the same deadline the hard drain uses; then
 * fpm_http_drain_start() runs by itself. Only a gateway with http.ready_path
 * is sent SIGUSR1. Idempotent. Event-loop context only. */
void fpm_http_drain_soft_start(struct fpm_http_gateway_s *gw);

/* Issue #646: the master's side. Sends SIGUSR1 to every gateway process of a
 * pool with http.ready_path, so the probe answers 503 from the first moment of
 * a stop or reload. Called from fpm_pctl() at the state change. */
void fpm_http_gateways_soft_drain(void);

/* Issue #646: 1 when some gateway with http.ready_path routes to wp. */
int fpm_http_pool_routed_by_ready_gateway(const struct fpm_worker_pool_s *wp);

/* Issue #646: 1 when the first signal of a stop or reload must wait for wp's
 * children: process_control_timeout > 0, a gateway with http.ready_path is
 * still alive, and it routes to wp. Read by fpm_pctl_kill_all(). */
int fpm_http_pool_held_for_window(const struct fpm_worker_pool_s *wp);

/* Issue #646: sends the signal that the first pass held back, to the pools
 * routed by a ready_path gateway, once no such gateway is alive, and re-arms
 * the master's timer for process_control_timeout from now. A no-op when
 * nothing is held. Defined in fpm_process_ctl.c. */
void fpm_pctl_release_deferred(void);

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
