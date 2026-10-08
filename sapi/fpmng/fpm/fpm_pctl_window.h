/* fpm-ng: the window of a stop or a reload, as the master holds it (issue #646).
 *
 * A gateway with http.ready_path keeps serving after the master starts a stop
 * or a reload (fpm_http_drain.h). The children of the pools that such a gateway
 * routes to must keep serving during that time, so the master holds back the
 * first signal for them. This file keeps that hold: which signal is held, when
 * a second stop signal overrides the hold, and which children may still fork.
 * fpm_process_ctl.c does the release (fpm_pctl_release_deferred()), because the
 * release needs its static timer and exit helpers.
 */
#ifndef FPM_PCTL_WINDOW_H
#define FPM_PCTL_WINDOW_H 1

#include "fpm_config.h"

#ifdef HAVE_FPM_HTTP

struct fpm_worker_pool_s;

/* The signal the first pass held back, or 0 when nothing is held. */
int fpm_pctl_window_held_signo(void);

/* Returns the held signal and clears the hold. The caller sends it. */
int fpm_pctl_window_take_held(void);

/* Forgets a hold from an earlier pass. Called at the first pass of each
 * stop or reload signal, before the pools are visited. */
void fpm_pctl_window_reset(void);

/* A second stop or reload signal arrived while one was in progress. Its signal
 * is not held: the operator asked for a faster stop, so the children get it at
 * once. Cleared for good: the master never returns to NORMAL state. */
void fpm_pctl_window_escalate(void);

/* 1 while the first pass holds a signal and a gateway with http.ready_path is
 * still alive. The master does not finish while this is 1. */
int fpm_pctl_window_open(void);

/* Called for every pool in the first pass of a stop or reload. stopping_first_pass
 * is 1 when the master is not in NORMAL state and no signal has gone out yet.
 * Returns 1 when wp's children are held back; the held signal is then signo. */
int fpm_pctl_window_hold_pool(const struct fpm_worker_pool_s *wp, int signo, int stopping_first_pass);

/* 1 when fpm_children_make() may fork a child of wp while the master is not in
 * NORMAL state. That holds only for an ondemand pool routed by a gateway with
 * http.ready_path, while the window is open: its new child then gets the
 * release signal with the others. An idle ondemand pool has no child, so a new
 * connection in the window would wait for a child that nobody starts. */
int fpm_pctl_may_fork_in_window(const struct fpm_worker_pool_s *wp);

#endif /* HAVE_FPM_HTTP */
#endif /* FPM_PCTL_WINDOW_H */
