/* See fpm_pctl_window.h for what the window is and why the master holds it (issue #646). */

#include "fpm_config.h"

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"

#ifdef HAVE_FPM_HTTP

#include "fpm_http_drain.h"
#include "fpm_pctl_window.h"

/* The signal that the first pass held back (0: nothing held). Set once per pool
 * by fpm_pctl_window_hold_pool(), so every held pool carries the same signal. */
static int fpm_window_held_signo = 0;

/* Set once a stop or reload was overridden by a further state change. */
static int fpm_window_escalated = 0;

int fpm_pctl_window_held_signo(void)
{
	return fpm_window_held_signo;
}

int fpm_pctl_window_take_held(void)
{
	int signo = fpm_window_held_signo;

	fpm_window_held_signo = 0;
	return signo;
}

void fpm_pctl_window_reset(void)
{
	fpm_window_held_signo = 0;
}

void fpm_pctl_window_escalate(void)
{
	fpm_window_escalated = 1;
}

int fpm_pctl_window_open(void)
{
	return fpm_window_held_signo != 0 && fpm_http_ready_gateways_alive();
}

int fpm_pctl_window_hold_pool(const struct fpm_worker_pool_s *wp, int signo, int stopping_first_pass)
{
	if (!stopping_first_pass || fpm_window_escalated || !fpm_http_pool_held_for_window(wp)) {
		return 0;
	}
	fpm_window_held_signo = signo;
	return 1;
}

int fpm_pctl_may_fork_in_window(const struct fpm_worker_pool_s *wp)
{
	return fpm_pctl_window_open() && wp->config->pm == PM_STYLE_ONDEMAND &&
		   fpm_http_pool_routed_by_ready_gateway(wp);
}

#endif /* HAVE_FPM_HTTP */
