/* fpm-ng: see fpm_http_header_cgi.h. */

#include "fpm_config.h"

#include <string.h>
#include <strings.h>

#include "fpm_http_header_cgi.h"

int fpm_http_header_cgi_key(const char *name, bool content_type_skip, char *out, size_t out_size) /* {{{ */
{
	size_t len = strlen(name);
	size_t i, j;

	/* Refused, not truncated or dropped: a name of up to
	 * FPM_HTTP_HEADER_NAME_MAX bytes (1024, itself generous next to Apache's
	 * 8190-byte whole-line LimitRequestFieldSize) is mapped, a longer one
	 * refuses the request -- audibly, so a silently missing header cannot
	 * come back. Issue #115. */
	if (len > FPM_HTTP_HEADER_NAME_MAX || out_size < len + sizeof("HTTP_")) {
		return -1;
	}
	/* Content-Length already has its CGI name at both callers; "Proxy" has
	 * none at all, and HTTP_PROXY is read as an outbound proxy by several
	 * client libraries (httpoxy, CVE-2016-5385). Core deletes the key from
	 * $_SERVER again -- or, when the process itself has an HTTP_PROXY
	 * environment variable, overwrites it with that value (check_http_proxy(),
	 * main/php_variables.c:861-874 in php-8.5.9) -- but getallheaders() reads
	 * the FastCGI parameters directly, not $_SERVER (sapi/fpm/fpm_main.c
	 * PHP_FUNCTION(apache_request_headers) -> fcgi_loadenv): measured on
	 * 192.168.8.50, 2026-09-09, `Proxy: attacker` produced no HTTP_PROXY in
	 * $_SERVER on either pool type yet answered {"proxy":"attacker", ...}
	 * without this exclusion. The exclusion is here so that a header the
	 * sibling transport refuses by name does not arrive because someone
	 * else's mitigation happens to cover one of the ways to read it.
	 * Issue #115. */
	if (strcasecmp(name, "Content-Length") == 0 || strcasecmp(name, "Proxy") == 0) {
		return 1;
	}
	/* A name with "_" would collide with its "-" spelling: the mapping below
	 * turns "-" into "_", so "X_Real_IP" and "X-Real-IP" both become
	 * HTTP_X_REAL_IP, and the last pair on the wire wins in $_SERVER
	 * (fcgi_hash_set replaces an existing key). A client could then override
	 * a header the reverse proxy in front set. nginx (default
	 * underscores_in_headers off) and Apache 2.4 drop such headers when they
	 * build the CGI environment; the gateway is the front server for this hop,
	 * so it does the same, and so does HTTP-direct. Issue #595. */
	if (memchr(name, '_', len) != NULL) {
		return 1;
	}
	if (strcasecmp(name, "Content-Type") == 0) {
		if (content_type_skip) {
			return 1;
		}
		memcpy(out, "CONTENT_TYPE", sizeof("CONTENT_TYPE"));
		return 0;
	}
	memcpy(out, "HTTP_", sizeof("HTTP_") - 1);
	j = sizeof("HTTP_") - 1;
	/* Explicit range, not toupper(): the CGI key a header lands under is a
	 * security boundary -- the Content-Length and Proxy exclusions above are
	 * enforced by name -- so the mapping must not depend on LC_CTYPE.
	 * Measured on 192.168.8.50, glibc 2.43, 2026-09-09: in tr_TR.UTF-8 and
	 * az_AZ.UTF-8 toupper('i') returns 'i' (the Turkish capital of 'i' is
	 * U+0130, which does not fit the single-byte table), so
	 * "If-Modified-Since" would become HTTP_IF_MODiFiED_SiNCE;
	 * de_DE.ISO-8859-1 remaps 30 bytes above 0x7F. Nothing calls setlocale()
	 * in the gateway process -- it translates HTTP to FastCGI and never
	 * executes application PHP -- but under pool.executor = worker the boot
	 * script calls setlocale() once and every later request's environment is
	 * derived inside that same PHP request, so the mapping must not depend on
	 * process state the application chose either. Issue #109; #105 replaced
	 * the identical construct on the HTTP-direct side. */
	for (i = 0; i < len; i++) {
		unsigned char ch = (unsigned char) name[i];

		if (ch >= 'a' && ch <= 'z') {
			out[j++] = (char) (ch - ('a' - 'A'));
		} else {
			out[j++] = ch == '-' ? '_' : (char) ch;
		}
	}
	out[j] = '\0';
	return 0;
}
/* }}} */
