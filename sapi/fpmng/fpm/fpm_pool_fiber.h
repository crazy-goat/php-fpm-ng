/* fpm-ng: pool.executor = fiber — EXPERIMENT.
 *
 * One process, many in-flight FastCGI requests, each in its own engine fiber
 * (Zend/zend_fibers.h: zend_fiber_start/resume/suspend are ZEND_API) on clean
 * upstream php-src — no fork and no patches. The scheduler is libevent (the
 * same library used by the HTTP gateway). The Fiber suspends in the stream
 * transport layer (php_stream_xport_register for "tcp"/"unix"), therefore only
 * on SOCKETS: fsockopen, mysqlnd, phpredis. sleep(), curl, libpq, and ordinary
 * files still block the whole process.
 *
 * Per-request state (SG, EG(symbol_table), ...) is swapped by the shared
 * fpm_pool_coop.c core. Rationale, limits, and results: docs/NOTES.md, 3u.
 */

#ifndef FPM_POOL_FIBER_H
#define FPM_POOL_FIBER_H 1

#include <sys/time.h>

struct fpm_worker_pool_s;

/* fpm_pool_type_s.validate — pm = static, NTS. */
int fpm_pool_fiber_validate(struct fpm_worker_pool_s *wp);

/* fpm_pool_type_s.child_main — libevent loop instead of a blocking accept loop.
 * Does not return. */
void fpm_pool_fiber_child_main(struct fpm_worker_pool_s *wp);

/* --- for fpm_pool_fiber_xport.c -------------------------------------------- */

/* Can the current code suspend the request fiber: we are in the request fiber
 * (not a nested user fiber), and switching is not blocked. */
int fpm_pool_fiber_can_wait(void);

/* Suspend the request fiber until the fd is ready (events: EV_READ and/or
 * EV_WRITE from libevent) or the timeout expires (NULL = no limit).
 * 1 = ready, 0 = timeout, -1 = cannot wait (caller must block). */
int fpm_pool_fiber_wait_fd(int fd, short events, struct timeval *timeout);

/* Wait for something other than an fd (for example, an evdns response). The
 * caller obtains the handle BEFORE starting the asynchronous operation and
 * gives it to the callback, which wakes the fiber through
 * fpm_pool_fiber_wake(). The callback may run synchronously, before wait_wake;
 * the caller detects that from its own state and does not wait. A wake outside
 * a wait is safe (no-op). wait_wake: 1 = woken, 0 = timeout (NULL = no limit),
 * -1 = cannot wait. */
void *fpm_pool_fiber_waiter(void);
int fpm_pool_fiber_wait_wake(struct timeval *timeout);
void fpm_pool_fiber_wake(void *waiter);

/* Scheduler event_base, for custom event sources on the same loop (evdns).
 * NULL outside a Fiber-executor child. */
struct event_base *fpm_pool_fiber_event_base(void);

/* Suspend the request fiber for `ms` milliseconds (ms < 0 = no limit).
 * Used by the TLS patch (0007): SSL_accept refuses to drive a renegotiation
 * that is already pending on the socket, so a post-handshake reneg request
 * is drained step by step with a guaranteed turn of the scheduler between
 * steps. 1 = elapsed/woken, -1 = cannot wait. */
int fpm_pool_fiber_sleep_ms(long ms);

/* Replace tcp/unix transports with the Fiber-suspending variant. Call once in
 * the child, after MINIT for all extensions (ext/openssl overwrites "tcp" in
 * its MINIT, so we must run AFTER it). */
void fpm_pool_fiber_xport_install(void);

#endif
