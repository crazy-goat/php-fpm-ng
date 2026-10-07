/* fpm-ng: decoration for a child that writes straight into the master's
 * error_log — issue #130.
 *
 * Upstream FPM takes error_log away from a child (fpm_stdio_init_child()) and
 * lets the master decorate whatever a child says, so zlog_buf_prefix() drops
 * the timestamp for any process with fpm_globals.is_child set. An fpmng HTTP
 * gateway process is forked outside that path (fpm_http_gateway_run()) and
 * keeps the inherited error_log fd, which is how a line like
 *
 *   WARNING: [pool web] http: upstream '127.0.0.1:9008' closed 2.0 ms after ...
 *
 * ended up in the same file as, and directly above,
 *
 *   [09-Sep-2026 21:00:49] WARNING: [pool web] child 2763991 exited on signal 9 ...
 *
 * with no time of its own (measured on 192.168.8.50, php-8.5.9, 2026-09-09).
 * Correlating the two — the whole point of the gateway line, issue #118 — was
 * then only possible by their order in the file, which several gateway
 * processes plus the master writing concurrently do not guarantee.
 *
 * How: not by reformatting the line here, which would mean reimplementing
 * vzlog()'s level filter, its errno suffix and its truncation, and losing the
 * ": %s (%d)" of every ZLOG_SYSERROR the way the relay in fpm_child_log.h
 * does. Instead the ONE input zlog_buf_prefix() reads — fpm_globals.is_child —
 * is cleared for the duration of the call, so upstream produces exactly the
 * line the master would have produced, pid and all.
 *
 * Coverage comes from the zlog() macro in our zlog.h, so an upstream file that
 * logs in a gateway process (fpm_unix.c during the privilege drop, say) is
 * decorated too, without a per-file include to remember.
 */

#ifndef FPM_CHILD_ERROR_LOG_H
#define FPM_CHILD_ERROR_LOG_H 1

/* Child, after fork(): this process logs into the master's error_log itself,
 * so its lines have to carry the master's decoration. Ignored — deliberately
 * silently — when this process has no error_log fd to write to, which is the
 * ordinary pool child and the error_log = syslog case; see the implementation.
 * Cannot fail. */
void fpm_child_error_log_use(void);

#endif
