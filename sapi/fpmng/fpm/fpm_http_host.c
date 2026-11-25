/* fpm-ng: host names of gateway requests for host-bound http.route[] (issue #658).
 * The grammar and the rules are documented in fpm_http_host.h. */
#include "fpm_config.h"

#include "fpm.h"
#include "fpm_http.h"

#ifdef HAVE_FPM_HTTP

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/keyvalq_struct.h>

#include "fpm_http_internal.h"
#include "fpm_http_host.h"

#define FPM_HTTP_HOST_NAME_MAX 253
#define FPM_HTTP_HOST_LABEL_MAX 63

/* ASCII only on purpose: isalnum() and tolower() depend on the locale, and a host
 * name that matches in one locale and not in another would route differently per
 * host. Every case fold in this file goes through host_fold(). */
static char host_fold(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char) (c + ('a' - 'A')) : c;
}

static int host_is_letter_or_digit(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

int fpm_http_host_valid_name(const char *name, size_t len)
{
	size_t i, label = 0;

	if (len == 0 || len > FPM_HTTP_HOST_NAME_MAX) {
		return 0;
	}
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char) name[i];

		if (c == '.') {
			/* An empty label (leading or doubled dot, or a trailing dot, which
			 * the Host grammar of this gateway does not accept) or a label that
			 * ends in '-' is refused. */
			if (label == 0 || name[i - 1] == '-') {
				return 0;
			}
			label = 0;
			continue;
		}
		if (!host_is_letter_or_digit(c) && c != '-') {
			return 0;
		}
		if (c == '-' && label == 0) {
			return 0;
		}
		if (++label > FPM_HTTP_HOST_LABEL_MAX) {
			return 0;
		}
	}
	return label != 0 && name[len - 1] != '-';
}

char *fpm_http_host_dup_lower(const char *name, size_t len)
{
	char *out = malloc(len + 1);
	size_t i;

	if (!out) {
		return NULL;
	}
	for (i = 0; i < len; i++) {
		out[i] = host_fold(name[i]);
	}
	out[len] = '\0';
	return out;
}

int fpm_http_host_equal(const char *a, const char *b, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (host_fold(a[i]) != host_fold(b[i])) {
			return 0;
		}
	}
	return 1;
}

int fpm_http_host_parse(const char *value, size_t len, char *out, size_t out_size)
{
	const char *colon;
	size_t host_len, i;

	while (len > 0 && (value[0] == ' ' || value[0] == '\t')) {
		value++;
		len--;
	}
	while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\t')) {
		len--;
	}
	/* An IPv6 literal cannot equal any configured name, so it names no host. */
	if (len == 0 || value[0] == '[') {
		return 0;
	}

	colon = memchr(value, ':', len);
	host_len = colon ? (size_t) (colon - value) : len;
	if (colon) {
		const char *p = colon + 1;
		size_t plen = len - host_len - 1, j;
		unsigned long port = 0;

		if (plen == 0 || plen > 5) {
			return 0;
		}
		for (j = 0; j < plen; j++) {
			if (p[j] < '0' || p[j] > '9') {
				return 0;
			}
			port = port * 10 + (unsigned long) (p[j] - '0');
		}
		if (port > 65535) {
			return 0;
		}
	}
	if (!fpm_http_host_valid_name(value, host_len) || host_len >= out_size) {
		return 0;
	}
	for (i = 0; i < host_len; i++) {
		out[i] = host_fold(value[i]);
	}
	out[host_len] = '\0';
	return 1;
}

int fpm_http_host_of_request(struct evhttp_request *req, char *out, size_t out_size)
{
	char authority[FPM_HTTP_AUTHORITY_MAX];
	struct evkeyval *kv;
	const char *host = NULL;
	unsigned count = 0;
	int have;

	have = fpm_http_absolute_authority(evhttp_request_get_uri(req), authority, sizeof(authority));
	if (have < 0) {
		return 0;
	}
	if (have > 0) {
		return fpm_http_host_parse(authority, strlen(authority), out, out_size);
	}

	/* evhttp_find_header() returns the first Host only. A second Host header
	 * is a malformed request, so it must not pick a host-bound route. */
	for (kv = TAILQ_FIRST(evhttp_request_get_input_headers(req)); kv; kv = TAILQ_NEXT(kv, next)) {
		if (strcasecmp(kv->key, "Host") == 0) {
			host = kv->value;
			count++;
		}
	}
	if (count != 1) {
		return 0;
	}
	return fpm_http_host_parse(host, strlen(host), out, out_size);
}

#endif /* HAVE_FPM_HTTP */
