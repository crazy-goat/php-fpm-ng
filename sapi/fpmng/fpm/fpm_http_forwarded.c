/* fpm-ng: see fpm_http_forwarded.h. */

#include "fpm_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <event2/http.h>

#include "fpm_http_forwarded.h"
#include "fpm_http_acl.h"

static void fpm_http_forwarded_trim_copy(const char *value, size_t len, char *out, size_t out_size) /* {{{ */
{
	while (len && (*value == ' ' || *value == '\t')) {
		value++;
		len--;
	}
	while (len && (value[len - 1] == ' ' || value[len - 1] == '\t')) {
		len--;
	}
	if (len >= out_size) {
		len = out_size - 1;
	}
	memcpy(out, value, len);
	out[len] = '\0';
}
/* }}} */

void fpm_http_forwarded_resolve(struct fpm_http_acl_s *trusted, const char *peer_addr, /* {{{ */
		struct evkeyvalq *headers, struct fpm_http_forwarded_result_s *out)
{
	const char *xff, *xfp, *xfport;

	memset(out, 0, sizeof(*out));
	out->scheme = "http"; /* bramka sama nigdy nie mowi TLS, patrz docs/NOTES.md sekcja 5 */

	if (!trusted || !fpm_http_acl_check(trusted, peer_addr)) {
		return; /* polaczenie nie jest z zaufanego proxy: X-Forwarded-* jest ignorowany */
	}

	xff = evhttp_find_header(headers, "X-Forwarded-For");
	if (xff && *xff) {
		const char *comma = strchr(xff, ',');
		size_t first_len = comma ? (size_t) (comma - xff) : strlen(xff);

		fpm_http_forwarded_trim_copy(xff, first_len, out->remote_addr, sizeof(out->remote_addr));
	}

	xfp = evhttp_find_header(headers, "X-Forwarded-Proto");
	if (xfp && *xfp) {
		if (strcasecmp(xfp, "https") == 0) {
			out->https = 1;
			out->scheme = "https";
		} else if (strcasecmp(xfp, "http") == 0) {
			out->https = 0;
			out->scheme = "http";
		}
		/* inna wartosc: nieznana, zostaje domyslne "http" -- nie ma czego zgadywac */
	}

	xfport = evhttp_find_header(headers, "X-Forwarded-Port");
	if (xfport && *xfport) {
		char *end;
		long port = strtol(xfport, &end, 10);

		if (!*end && port > 0 && port <= 65535) {
			snprintf(out->server_port, sizeof(out->server_port), "%ld", port);
		}
		/* niepoprawna wartosc: server_port zostaje pusty, wolajacy uzyje wlasnego portu */
	}
}
/* }}} */
