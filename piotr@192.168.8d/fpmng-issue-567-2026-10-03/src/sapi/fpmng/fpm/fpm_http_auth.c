/* fpm-ng: see fpm_http_auth.h. */

#include "fpm_config.h"

#include <string.h>
#include <strings.h>

#include "fpm_http_auth.h"

static int fpm_http_b64_val(unsigned char c) /* {{{ */
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}
/* }}} */

/* Decode into `out` (size out_size, NUL-terminated), silently truncating when
 * it does not fit. Tolerant of input noise (spaces and so on), like most
 * base64 decoders used for Basic Auth. */
static void fpm_http_b64_decode(const char *in, size_t in_len, char *out, size_t out_size) /* {{{ */
{
	size_t o = 0, i;
	int vals[4], n = 0;

	if (out_size == 0) {
		return;
	}

	for (i = 0; i < in_len && in[i] != '='; i++) {
		int v = fpm_http_b64_val((unsigned char) in[i]);

		if (v < 0) {
			continue;
		}
		vals[n++] = v;
		if (n == 4) {
			if (o < out_size - 1) out[o++] = (char) ((vals[0] << 2) | (vals[1] >> 4));
			if (o < out_size - 1) out[o++] = (char) ((vals[1] << 4) | (vals[2] >> 2));
			if (o < out_size - 1) out[o++] = (char) ((vals[2] << 6) | vals[3]);
			n = 0;
		}
	}
	if (n >= 2 && o < out_size - 1) {
		out[o++] = (char) ((vals[0] << 2) | (vals[1] >> 4));
	}
	if (n >= 3 && o < out_size - 1) {
		out[o++] = (char) ((vals[1] << 4) | (vals[2] >> 2));
	}
	out[o] = '\0';
}
/* }}} */

void fpm_http_auth_parse(const char *authorization_header, /* {{{ */
		char auth_type[FPM_HTTP_AUTH_TYPE_LEN], char remote_user[FPM_HTTP_AUTH_USER_LEN])
{
	const char *space;
	size_t scheme_len;

	auth_type[0] = '\0';
	remote_user[0] = '\0';

	if (!authorization_header || !*authorization_header) {
		return;
	}

	space = strchr(authorization_header, ' ');
	scheme_len = space ? (size_t) (space - authorization_header) : strlen(authorization_header);
	if (scheme_len == 0 || scheme_len >= FPM_HTTP_AUTH_TYPE_LEN) {
		return; /* empty or implausibly long scheme: do not guess */
	}
	memcpy(auth_type, authorization_header, scheme_len);
	auth_type[scheme_len] = '\0';

	if (space && strcasecmp(auth_type, "Basic") == 0) {
		const char *b64 = space + 1;
		char decoded[FPM_HTTP_AUTH_USER_LEN + 256]; /* user + ':' + password */
		const char *colon;

		while (*b64 == ' ' || *b64 == '\t') {
			b64++;
		}
		fpm_http_b64_decode(b64, strlen(b64), decoded, sizeof(decoded));

		colon = strchr(decoded, ':');
		if (colon) {
			size_t user_len = (size_t) (colon - decoded);

			if (user_len >= FPM_HTTP_AUTH_USER_LEN) {
				user_len = FPM_HTTP_AUTH_USER_LEN - 1;
			}
			memcpy(remote_user, decoded, user_len);
			remote_user[user_len] = '\0';
		}
		/* No colon after decoding: malformed header; keep AUTH_TYPE but do not
		 * set REMOTE_USER, as Apache would do. */
	}
	/* Other schemes (Bearer, Digest, Negotiate, ...): only AUTH_TYPE, as in any
	 * other CGI deployment — token decoding is the application's responsibility. */
}
/* }}} */
