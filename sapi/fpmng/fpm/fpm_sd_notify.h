/* fpm-ng: issue #643 -- native sd_notify(3) for a systemd Type=notify unit.
 * See fpm_sd_notify.c for the protocol, the one-process rule and the defaults.
 *
 * Every function here is a no-op when NOTIFY_SOCKET is not set, so a master
 * started by hand, in a container or under OpenRC behaves exactly as before.
 * Nothing here links libsystemd: the datagram is written with plain sendto(),
 * as the packaged binary must need nothing but the distribution's libphp.
 *
 * READY=1 means one thing only: the master's listening sockets are bound and
 * the initial children are forked. It does NOT mean that a gateway, an
 * http-direct pool or a FastCGI pool already accepts a request. A gateway
 * child can still be starting when the unit becomes "active". */

#ifndef FPM_SD_NOTIFY_H
#define FPM_SD_NOTIFY_H 1

/* READY=1. Called once per generation, in the master, after every pool has
 * bound its listening socket (fpm.c, fpm_run()). A new generation after a
 * SIGUSR2 reload sends it again, which is what ends systemd's "reloading"
 * state. */
void fpm_sd_notify_ready(void);

/* RELOADING=1 with MONOTONIC_USEC. Called from fpm_pctl() when a reload has
 * passed the configuration check and the state changes to 'reloading'. */
void fpm_sd_notify_reloading(void);

/* RELOADING=1 with MONOTONIC_USEC, then READY=1, for a reload the configuration
 * check refused. Called from fpm_pctl() instead of fpm_sd_notify_reloading().
 * The master keeps its current generation, so READY=1 is true, and the pair
 * ends the reload job that systemd started (Type=notify-reload waits for
 * READY=1 after ReloadSignal=). A bare READY=1 does not end the job. The -t
 * line of the unit catches most broken configurations before the signal; this
 * covers the rest, so systemd does not wait for TimeoutStartSec. */
void fpm_sd_notify_reload_refused(void);

/* STOPPING=1. Called from fpm_pctl() when the master enters 'finishing' or
 * 'terminating' (SIGQUIT, SIGINT, SIGTERM, or a failed start). */
void fpm_sd_notify_stopping(void);

/* WATCHDOG=1 at a period of half WATCHDOG_USEC, which systemd sets when the
 * unit has WatchdogSec=. Off when WATCHDOG_USEC is unset, which is the packaged
 * default. Sends one WATCHDOG=1 at once, then one per period from the master's
 * event loop, so a master that stops running its loop stops being pinged. */
void fpm_sd_notify_watchdog_start(void);

#endif
