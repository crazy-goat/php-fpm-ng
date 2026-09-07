/* fpm-ng: pool.executor = fiber — surface of fpm_pool_fiber_xport.c used by
 * the TLS patch (patches/0007-fiber-tls-*.patch, gated on HAVE_FPMNG_FIBER_TLS).
 *
 * The patch lives in ext/openssl and needs two things from here: the ops
 * wrapper (so TLS reads/writes/connects suspend the fiber exactly like plain
 * tcp) and the install hook. Keeping this in a separate header makes the
 * patch's dependency on our tree explicit at its #include line. */

#ifndef FPM_POOL_FIBER_XPORT_H
#define FPM_POOL_FIBER_XPORT_H 1

#include "php_streams.h"

/* Wrap an ops table with the fiber-suspending variant (read/write/connect
 * wait on the scheduler instead of blocking). NULL = the wrapper map is full
 * — never happens for the TLS patch, which has a slot reserved
 * (FPM_FIBER_OPS_MAX in the .c file). */
const php_stream_ops *fpm_fiber_xport_wrap(const php_stream_ops *orig);

/* Wrap the ssl/sslv3/tls/tlsv1.x transports. Called from
 * fpm_pool_fiber_xport_install() under HAVE_FPMNG_FIBER_TLS. Its log line is
 * the "stream transports hooked: ssl=..." one. */
void fpm_fiber_tls_xport_install(void);

#endif
