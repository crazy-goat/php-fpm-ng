/* fpm-ng: PHP's own diagnostics (errors, warnings, uncaught exceptions,
 * error_log() from user code) into FPM's error_log, for a pool type whose
 * policy runs in the child.
 *
 * A classic FastCGI child has two places to put a PHP error and an operator
 * who chose between them: the response (display_errors) and the FastCGI stderr
 * stream, which the front-end web server writes into its own log. A
 * pool.type = supervisor or pool.type = cron child has neither. It serves no
 * request, so "the response" is the process's stdout — which
 * fpm_stdio_init_main() has pointed at /dev/null unless the pool also asked for
 * catch_workers_output — and there is no front end to hand a stderr stream to.
 *
 * MEASURED before this file (issue #124, test box, php-8.5.9): a supervisor
 * script calling an undefined function produced NOTHING in error_log; with
 * catch_workers_output = yes it produced five master-side
 * WARNING "child N said into stdout" lines carrying <br /> and <b> markup,
 * because a child that runs with the built-in INI defaults has
 * display_errors = 1, html_errors = 1 and log_errors = 0 (main/main.c) — so the
 * fatal was displayed, in HTML, on a stdout nobody reads, and logged nowhere.
 *
 * Two changes, both only for a type with child_logs_via_master:
 *
 * - INI defaults for the child: log_errors on, display_errors and html_errors
 *   off. Applied through fpm_php_apply_defines_ex() — the same call that
 *   applies php_value — so they alter the ini entry's MASTER value and
 *   therefore survive every php_request_startup() of every script run, instead
 *   of being restored at the end of the first one. Applied from
 *   fpm_child_init() BEFORE fpm_php_init_child(), so php_value /
 *   php_admin_value in the pool configuration still override them.
 *
 *   PRECISELY: they override php.ini, and only pool-level php_value /
 *   php_admin_value override them. fpm_php_apply_defines_ex() writes the ini
 *   entry's master value and cannot tell "still the compiled-in default" from
 *   "the operator set this in php.ini", so display_errors = On in php.ini stops
 *   reaching a supervisor pool's stdout once this is in place. That is the
 *   deliberate trade: the value it had there was chosen for pools that have a
 *   response to display errors in, this pool type has none, and a pool that
 *   really wants the console copy back sets php_value[display_errors] = 1 in
 *   its own section.
 * - sapi_module.log_message is replaced so that the message reaches zlog() at
 *   a level derived from the error's severity (LOG_ERR -> ZLOG_ERROR, and so
 *   on) instead of upstream's fixed ZLOG_NOTICE (sapi_cgi_log_message(),
 *   fpm_main.c). From there it takes the channel of issue #121
 *   (fpm_child_log.h) and lands in error_log as an ordinary master line.
 *
 * error_log (the PHP INI directive) is deliberately NOT touched: when it is
 * set, php_log_err() writes to that file and never calls the SAPI at all, which
 * is exactly what an operator who set it asked for.
 */

#ifndef FPM_CHILD_PHP_LOG_H
#define FPM_CHILD_PHP_LOG_H 1

struct fpm_worker_pool_s;

/* Child, from fpm_child_init(), before fpm_php_init_child(). Does nothing for
 * a pool type that does not opt in. Cannot fail in a way worth refusing to
 * start the supervised process over — every failure is logged and leaves the
 * previous (upstream) behaviour in place. */
void fpm_child_php_log_init_child(struct fpm_worker_pool_s *wp);

#endif
