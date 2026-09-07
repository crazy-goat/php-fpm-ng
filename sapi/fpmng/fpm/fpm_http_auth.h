/* fpm-ng: AUTH_TYPE / REMOTE_USER for the HTTP gateway (fpm_http.c).
 *
 * Derived from the Authorization header, exactly as any CGI/FastCGI server
 * would do it. The gateway does NOT authenticate anything itself; it only
 * passes the header's information to the application:
 *   - "Basic <base64>"  -> AUTH_TYPE=Basic, REMOTE_USER = user from "user:pass"
 *   - any other scheme -> only AUTH_TYPE, REMOTE_USER is not set
 *
 * Custom base64 decoder: the gateway is a separate process without initialized
 * Zend MM (see docs/NOTES.md, the pool-type section), so php_base64_decode_ex()
 * from ext/standard is not safe to call here.
 */

#ifndef FPM_HTTP_AUTH_H
#define FPM_HTTP_AUTH_H 1

#define FPM_HTTP_AUTH_TYPE_LEN 32
#define FPM_HTTP_AUTH_USER_LEN 256

/* authorization_header may be NULL (no header) -- both buffers then remain
 * empty. Both buffers are always NUL-terminated; empty [0] == '\0' means
 * "do not set this CGI parameter". */
void fpm_http_auth_parse(const char *authorization_header,
	char auth_type[FPM_HTTP_AUTH_TYPE_LEN], char remote_user[FPM_HTTP_AUTH_USER_LEN]);

#endif
