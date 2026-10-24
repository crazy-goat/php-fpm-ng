/*
 * libFuzzer harness for the WebSocket upgrade checks in fpm_http_direct_ws.c
 * (issue #995): fpm_ws_key_valid() and fpm_ws_header_has_token().
 *
 * Input layout:
 *   byte 0, bit 0   0 = check Sec-WebSocket-Key, 1 = check a header token
 *   byte 1, bit 0   for a token: 0 = Connection: upgrade, 1 = Upgrade: websocket
 *   bytes 2..       key mode: the key string; token mode: header lines,
 *                   "Name: value", separated by '\n'
 *
 * Key mode checks against a reference: a key is valid only when it is the
 * canonical base64 of 16 bytes, which is the form RFC 6455 requires. The
 * reference decodes the key and encodes it again; the strings must match.
 *
 * Token mode checks against a reference that splits each value on ',' and
 * trims spaces and tabs, then compares case-insensitively.
 */
#include "fpm_http_direct_ws.c"

/*
 * The worker state that the upgrade-failure paths of this file read. The
 * handshake checks never reach those paths, so the zeroed state is not used.
 */
struct fpm_worker_state fw;

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/queue.h>

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void encode16(const unsigned char in[16], char out[25])
{
	size_t i, o = 0;

	for (i = 0; i < 15; i += 3) {
		unsigned v = ((unsigned) in[i] << 16) | ((unsigned) in[i + 1] << 8) | in[i + 2];

		out[o++] = b64[(v >> 18) & 63];
		out[o++] = b64[(v >> 12) & 63];
		out[o++] = b64[(v >> 6) & 63];
		out[o++] = b64[v & 63];
	}
	out[o++] = b64[((unsigned) in[15] >> 2) & 63];
	out[o++] = b64[((unsigned) in[15] & 3) << 4];
	out[o++] = '=';
	out[o++] = '=';
	out[o] = '\0';
}

static int ref_key_valid(const char *key)
{
	unsigned char bytes[16];
	char again[25];
	unsigned acc = 0;
	int bits = 0;
	size_t i, nb = 0;

	if (strlen(key) != 24 || key[22] != '=' || key[23] != '=') {
		return 0;
	}
	for (i = 0; i < 22; i++) {
		const char *p = strchr(b64, key[i]);

		if (!p || key[i] == '\0') {
			return 0;
		}
		acc = (acc << 6) | (unsigned) (p - b64);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (nb >= 16) {
				return 0;
			}
			bytes[nb++] = (unsigned char) ((acc >> bits) & 0xff);
		}
	}
	if (nb != 16) {
		return 0;
	}
	encode16(bytes, again);
	return memcmp(again, key, 24) == 0;
}

static int ref_has_token(struct evkeyvalq *headers, const char *name, const char *token)
{
	struct evkeyval *kv;
	size_t tlen = strlen(token);

	TAILQ_FOREACH(kv, headers, next) {
		const char *p;

		if (strcasecmp(kv->key, name) != 0) {
			continue;
		}
		p = kv->value;
		for (;;) {
			const char *end = strchr(p, ',');
			const char *s = p, *e = end ? end : p + strlen(p);

			while (s < e && (*s == ' ' || *s == '\t')) {
				s++;
			}
			while (e > s && (e[-1] == ' ' || e[-1] == '\t')) {
				e--;
			}
			if ((size_t) (e - s) == tlen && strncasecmp(s, token, tlen) == 0) {
				return 1;
			}
			if (!end) {
				break;
			}
			p = end + 1;
		}
	}
	return 0;
}

static void fail(const char *what)
{
	fprintf(stderr, "fuzz_ws_handshake: %s\n", what);
	abort();
}

static void check_key(const uint8_t *payload, size_t len)
{
	char *key = malloc(len + 1);
	int got, want;

	if (!key) {
		return;
	}
	memcpy(key, payload, len);
	key[len] = '\0';
	got = fpm_ws_key_valid(key) ? 1 : 0;
	want = ref_key_valid(key);
	if (got != want) {
		fprintf(stderr, "fuzz_ws_handshake: key \"%s\": production %d, reference %d\n", key, got, want);
		abort();
	}
	free(key);
}

static void check_tokens(const uint8_t *payload, size_t len, const char *name, const char *token)
{
	struct evkeyvalq headers;
	size_t off = 0;
	int got, want;

	TAILQ_INIT(&headers);
	while (off < len) {
		size_t end = off;
		char *line, *colon;

		while (end < len && payload[end] != '\n') {
			end++;
		}
		line = malloc(end - off + 1);
		if (!line) {
			abort();
		}
		memcpy(line, payload + off, end - off);
		line[end - off] = '\0';
		colon = strchr(line, ':');
		if (colon) {
			*colon = '\0';
			evhttp_add_header(&headers, line, colon + 1);
		}
		free(line);
		off = end + 1;
	}
	got = fpm_ws_header_has_token(&headers, name, token) ? 1 : 0;
	want = ref_has_token(&headers, name, token);
	evhttp_clear_headers(&headers);
	if (got != want) {
		fprintf(stderr, "fuzz_ws_handshake: %s: %s: production %d, reference %d\n", name, token, got, want);
		abort();
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 2) {
		return 0;
	}
	if (!(data[0] & 1)) {
		check_key(data + 2, size - 2);
	} else if (data[1] & 1) {
		check_tokens(data + 2, size - 2, "Upgrade", "websocket");
	} else {
		check_tokens(data + 2, size - 2, "Connection", "upgrade");
	}
	return 0;
}
