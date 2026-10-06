/* fpm-ng: pool.executor = fiber — surface of fpm_pool_fiber_xport.c used by
 * the TLS patch (async/patches/0007-fiber-tls-*.patch, gated on HAVE_FPMNG_FIBER_TLS).
 *
 * The patch lives in ext/openssl and needs two things from here: the ops
 * wrapper (so TLS reads/writes/connects suspend the fiber exactly like plain
 * tcp) and the install hook. Keeping this in a separate header makes the
 * patch's dependency on our tree explicit at its #include line. */

#ifndef FPM_POOL_FIBER_XPORT_H
#define FPM_POOL_FIBER_XPORT_H 1

#include <sys/time.h>

#include "php_streams.h"
#include "php_network.h"

#include "fpm_pool_fiber_io.h"

/* Registry entry "xport" (fpm_pool_fiber_intercept.c). */
extern struct fpm_fiber_intercept_s fpm_fiber_xport_intercept;

/* For patch 0007: may the TLS code suspend right now (a request fiber, and
 * the "xport" entry is not disabled)? Non-zero = yes. */
int fpm_fiber_xport_tls_active(void);

/* For patch 0007: php_pollfd_for() replacement. poll_events are poll(2) bits
 * (POLLIN/POLLOUT/POLLPRI); 1 ready, 0 timeout, -1 did not wait — the caller
 * then polls exactly as upstream. */
int fpm_fiber_xport_tls_wait(php_socket_t fd, int poll_events, struct timeval *timeout);

/* Wrap an ops table with the fiber-suspending variant (read/write/connect
 * wait on the scheduler instead of blocking). NULL = the wrapper map is full
 * — never happens for the TLS patch, which has a slot reserved
 * (FPM_FIBER_OPS_MAX in the .c file). */
const php_stream_ops *fpm_fiber_xport_wrap(const php_stream_ops *orig);

/* Wrap the ssl/sslv3/tls/tlsv1.x transports. Called from
 * the "xport" entry's install under HAVE_FPMNG_FIBER_TLS. Its log line is
 * the "stream transports hooked: ssl=..." one. */
void fpm_fiber_tls_xport_install(void);

#endif
