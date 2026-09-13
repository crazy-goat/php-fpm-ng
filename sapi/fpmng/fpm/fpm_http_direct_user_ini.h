/* fpm-ng: .user.ini activation for HTTP-direct pools (issue #60).
 * See fpm_http_direct_user_ini.c. */

#ifndef FPM_HTTP_DIRECT_USER_INI_H
#define FPM_HTTP_DIRECT_USER_INI_H 1

#include <limits.h>

/* Called once per child, after fpm_http_direct_resolve_script() has produced
 * the pair this child will serve for its whole life. Both paths come from the
 * pool configuration; nothing a client sends reaches this function, which is
 * the whole security argument of issue #60.
 *
 * `pool` is used for log lines only.
 *
 * Returns -1 for the one configuration this transport refuses outright -- a
 * user_ini.filename carrying a path separator, which would let the scan leave
 * the document root -- and the caller must exit FPM_EXIT_CONFIG. Every other
 * problem returns 0 with the feature disabled and a log line: a pool that
 * refuses to serve because it could not read an optional ini file is a worse
 * outcome than one that serves with php.ini alone.
 */
int fpm_http_direct_user_ini_init_child(const char *pool, const char *root, const char *script);

/* The sapi_module.pre_request_init handler. Installed by both executors, so
 * the activation happens exactly where the CGI SAPI does it: inside
 * sapi_activate(), i.e. after zend_activate() has reset whatever the previous
 * request modified and before the script runs. */
int fpm_http_direct_user_ini_pre_request(void);

/* Releases the per-child cache. Only the exit paths call it: a child that is
 * about to _exit() does not need it, but a child that returns through
 * php_module_shutdown() under a leak checker does. */
void fpm_http_direct_user_ini_shutdown_child(void);

#endif
