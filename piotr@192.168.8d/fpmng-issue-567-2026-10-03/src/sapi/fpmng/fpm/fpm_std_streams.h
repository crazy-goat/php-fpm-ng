/* fpm-ng: STDIN, STDOUT and STDERR for the process types in this SAPI where a
 * PHP script owns the process instead of a request. Extracted from
 * fpm_pool_script.c (issue #126) when pool.executor = worker needed the same
 * three constants for the same reason (issue #73).
 */

#ifndef FPM_STD_STREAMS_H
#define FPM_STD_STREAMS_H 1

/* Registers STDIN, STDOUT and STDERR as the CLI SAPI does. Must run after
 * php_request_startup(): php_stream_to_zval() puts a resource in
 * EG(regular_list), which init_executor() creates.
 *
 * The streams are ordinary request-scoped ones, so the caller decides the
 * lifetime by choosing when to call this — see the comment on the definition
 * for why that is per iteration for a script-running pool and once for a
 * worker. pool_name is used only for the log line a failure produces.
 */
void fpm_std_streams_register(const char *pool_name);

#endif
