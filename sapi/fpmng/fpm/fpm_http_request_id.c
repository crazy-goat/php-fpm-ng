/* fpm-ng: see fpm_http_request_id.h. */

#include "fpm_config.h"

#include <string.h>
#include <sys/random.h>
#include <unistd.h> /* musl declares getentropy() here, not in <sys/random.h> */

#include "fpm_http_request_id.h"

#define FPM_HTTP_REQUEST_ID_RAW_BYTES 16 /* 128 bits */

bool fpm_http_request_id_generate(char out[FPM_HTTP_REQUEST_ID_SIZE]) /* {{{ */
{
	static const char hex[] = "0123456789abcdef";
	unsigned char raw[FPM_HTTP_REQUEST_ID_RAW_BYTES];
	size_t i;

	out[0] = '\0';
	if (getentropy(raw, sizeof(raw)) != 0) {
		return false;
	}
	for (i = 0; i < sizeof(raw); i++) {
		out[2 * i] = hex[raw[i] >> 4];
		out[2 * i + 1] = hex[raw[i] & 0x0f];
	}
	out[2 * sizeof(raw)] = '\0';
	return true;
}
/* }}} */

bool fpm_http_request_id_valid(const char *id) /* {{{ */
{
	size_t i, len;

	if (!id) {
		return false;
	}
	len = strlen(id);
	if (len == 0 || len > FPM_HTTP_REQUEST_ID_MAX) {
		return false;
	}
	for (i = 0; i < len; i++) {
		char c = id[i];
		int ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-';

		if (!ok) {
			return false;
		}
	}
	return true;
}
/* }}} */
