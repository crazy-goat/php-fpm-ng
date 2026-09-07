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
	out->scheme = "http"; /* the gateway never claims TLS; see docs/NOTES.md section 5 */

	if (!trusted || !fpm_http_acl_check(trusted, peer_addr)) {
		return; /* connection is not from a trusted proxy: ignore X-Forwarded-* */
	}

	xff = evhttp_find_header(headers, "X-Forwarded-For");
	if (xff && *xff) {
		/* Walk from the RIGHT, skipping addresses that are themselves trusted
		 * proxies, and take the first one that is not — it is the only element
		 * the client could not forge. Taking the first from the left is a hole:
		 * nginx's default $proxy_add_x_forwarded_for APPENDS the client address
		 * to what the client sent, so the left side comes directly from the
		 * client. This works for one proxy and for a chain. */
		const char *end = xff + strlen(xff);

		while (end > xff) {
			const char *start = end;
			char candidate[FPM_HTTP_FORWARDED_ADDR_LEN];

			while (start > xff && start[-1] != ',') {
				start--;
			}
			fpm_http_forwarded_trim_copy(start, (size_t) (end - start), candidate, sizeof(candidate));

			if (candidate[0] && !fpm_http_acl_check(trusted, candidate)) {
				memcpy(out->remote_addr, candidate, sizeof(candidate));
				break;
			}
			/* Empty element or a trusted proxy: continue to the left. */
			end = (start > xff) ? start - 1 : xff;
		}
		/* All elements trusted (or the list is empty): there is nothing to
		 * override REMOTE_ADDR with; keep the direct peer address. */
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
		/* Other value: unknown; keep the "http" default — do not guess. */
	}

	xfport = evhttp_find_header(headers, "X-Forwarded-Port");
	if (xfport && *xfport) {
		char *end;
		long port = strtol(xfport, &end, 10);

		if (!*end && port > 0 && port <= 65535) {
			snprintf(out->server_port, sizeof(out->server_port), "%ld", port);
		}
		/* Invalid value: leave server_port empty; the caller uses its own port. */
	}
}
/* }}} */
