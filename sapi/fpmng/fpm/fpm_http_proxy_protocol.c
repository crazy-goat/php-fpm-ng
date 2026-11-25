/* fpm-ng: see fpm_http_proxy_protocol.h. */
#include "fpm_config.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include "fpm_http_proxy_protocol.h"

/* v2 signature. Its first byte is 0x0D, which no HTTP method starts with, so a
 * plain HTTP client is refused on its first bytes (see proxy_parse_v2). */
static const unsigned char proxy_v2_sig[12] = { 0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A };

/* Port in a v1 token: decimal digits only, at most five of them, value at most
 * 65535. The specification allows nothing else (no sign, no spaces, no hex),
 * and a stricter reader than the sender's is fine: a malformed header is
 * refused, never guessed at. */
static int proxy_v1_port(const unsigned char *tok, size_t len, unsigned short *out)
{
	unsigned long value = 0;
	size_t i;

	if (len == 0 || len > 5) {
		return 0;
	}
	for (i = 0; i < len; i++) {
		if (tok[i] < '0' || tok[i] > '9') {
			return 0;
		}
		value = value * 10 + (tok[i] - '0');
	}
	if (value > 65535) {
		return 0;
	}
	*out = (unsigned short) value;
	return 1;
}

/* Converts one v1 address token to binary. inet_pton() needs a C string, and
 * the longest valid text address (IPv6 with an embedded IPv4) fits in
 * INET6_ADDRSTRLEN, so a longer token cannot be valid. */
static int proxy_v1_addr(int family, const unsigned char *tok, size_t len, void *bin)
{
	char text[INET6_ADDRSTRLEN];

	if (len == 0 || len >= sizeof(text)) {
		return 0;
	}
	memcpy(text, tok, len);
	text[len] = '\0';
	return inet_pton(family, text, bin) == 1;
}

/* Reads the token at *p, up to the next space or end. Returns 0 when the token
 * is empty (a double space, or a missing field). The space is not consumed:
 * proxy_v1_sep() does that, so a trailing space stays visible to the caller. */
static int proxy_v1_token(const unsigned char **p, const unsigned char *end, const unsigned char **tok, size_t *len)
{
	const unsigned char *start = *p;

	while (*p < end && **p != ' ') {
		(*p)++;
	}
	*tok = start;
	*len = (size_t) (*p - start);
	return *len > 0;
}

/* Consumes one separator space. Returns 0 when there is none. */
static int proxy_v1_sep(const unsigned char **p, const unsigned char *end)
{
	if (*p < end && **p == ' ') {
		(*p)++;
		return 1;
	}
	return 0;
}

/* The v1 line between "PROXY " and CRLF: "UNKNOWN [anything]" or
 * "TCP4|TCP6 <src> <dst> <sport> <dport>". Anything else is refused. */
static enum fpm_http_proxy_status proxy_v1_line(const unsigned char *p, const unsigned char *end, struct fpm_http_proxy_header_s *out)
{
	const unsigned char *tok;
	size_t tlen;
	int family;
	unsigned char src[sizeof(struct in6_addr)], dst[sizeof(struct in6_addr)];
	unsigned short sport, dport;

	if ((size_t) (end - p) >= 7 && memcmp(p, "UNKNOWN", 7) == 0 && (end - p == 7 || p[7] == ' ')) {
		out->has_source = 0;
		return FPM_HTTP_PROXY_DONE;
	}
	if ((size_t) (end - p) >= 5 && memcmp(p, "TCP4 ", 5) == 0) {
		family = AF_INET;
	} else if ((size_t) (end - p) >= 5 && memcmp(p, "TCP6 ", 5) == 0) {
		family = AF_INET6;
	} else {
		return FPM_HTTP_PROXY_BAD;
	}
	p += 5;
	if (!proxy_v1_token(&p, end, &tok, &tlen) || !proxy_v1_addr(family, tok, tlen, src)) {
		return FPM_HTTP_PROXY_BAD;
	}
	if (!proxy_v1_sep(&p, end) || !proxy_v1_token(&p, end, &tok, &tlen) || !proxy_v1_addr(family, tok, tlen, dst)) {
		return FPM_HTTP_PROXY_BAD;
	}
	if (!proxy_v1_sep(&p, end) || !proxy_v1_token(&p, end, &tok, &tlen) || !proxy_v1_port(tok, tlen, &sport)) {
		return FPM_HTTP_PROXY_BAD;
	}
	if (!proxy_v1_sep(&p, end) || !proxy_v1_token(&p, end, &tok, &tlen) || !proxy_v1_port(tok, tlen, &dport)) {
		return FPM_HTTP_PROXY_BAD;
	}
	if (p != end) {
		/* a fifth token, or a trailing space */
		return FPM_HTTP_PROXY_BAD;
	}
	if (!inet_ntop(family, src, out->addr, sizeof(out->addr))) {
		return FPM_HTTP_PROXY_BAD;
	}
	out->has_source = 1;
	out->port = sport;
	return FPM_HTTP_PROXY_DONE;
}

/* v1 is one text line. Its CRLF must be inside the first 107 bytes, and a bare
 * LF is refused: the specification requires CRLF, and accepting LF would let
 * two readers disagree about where the header ends. */
static enum fpm_http_proxy_status proxy_parse_v1(const unsigned char *buf, size_t len, struct fpm_http_proxy_header_s *out)
{
	static const char prefix[] = "PROXY ";
	size_t window = len < FPM_HTTP_PROXY_V1_MAX ? len : FPM_HTTP_PROXY_V1_MAX;
	size_t i, n = len < sizeof(prefix) - 1 ? len : sizeof(prefix) - 1;
	enum fpm_http_proxy_status status;
	struct fpm_http_proxy_header_s tmp;

	if (memcmp(buf, prefix, n) != 0) {
		return FPM_HTTP_PROXY_BAD;
	}
	if (len < sizeof(prefix) - 1) {
		return FPM_HTTP_PROXY_NEED_MORE;
	}
	for (i = 0; i < window && buf[i] != '\n'; i++) {
	}
	if (i == window) {
		return len < FPM_HTTP_PROXY_V1_MAX ? FPM_HTTP_PROXY_NEED_MORE : FPM_HTTP_PROXY_BAD;
	}
	if (i == 0 || buf[i - 1] != '\r') {
		return FPM_HTTP_PROXY_BAD;
	}
	memset(&tmp, 0, sizeof(tmp));
	status = proxy_v1_line(buf + sizeof(prefix) - 1, buf + i - 1, &tmp);
	if (status != FPM_HTTP_PROXY_DONE) {
		return status;
	}
	tmp.length = i + 1;
	*out = tmp;
	return FPM_HTTP_PROXY_DONE;
}

/* v2 is a 16-byte fixed part, then a block of body_length bytes. The fixed
 * part holds the signature, version and command (high and low nibble of byte
 * 12), the address family (high nibble of byte 13) and body_length (bytes 14
 * and 15, big endian). The addresses follow, then TLVs, which are skipped.
 * The transport protocol is the low nibble of byte 13 (haproxy
 * proxy-protocol.txt section 2.2): 0 UNSPEC and 1 STREAM are accepted, 2 DGRAM
 * and 3..F are refused, because the listener is TCP (issue #651, review R3). */
static enum fpm_http_proxy_status proxy_parse_v2(const unsigned char *buf, size_t len, struct fpm_http_proxy_header_s *out)
{
	size_t n = len < sizeof(proxy_v2_sig) ? len : sizeof(proxy_v2_sig);
	unsigned version, command, family, protocol;
	size_t body, total;
	struct fpm_http_proxy_header_s tmp;

	if (memcmp(buf, proxy_v2_sig, n) != 0) {
		return FPM_HTTP_PROXY_BAD;
	}
	if (len < 16) {
		return FPM_HTTP_PROXY_NEED_MORE;
	}
	version = buf[12] >> 4;
	command = buf[12] & 0x0F;
	family = buf[13] >> 4;
	protocol = buf[13] & 0x0F;
	body = ((size_t) buf[14] << 8) | buf[15];
	if (version != 2 || command > 1 || family > 3 || protocol > 1 || body > FPM_HTTP_PROXY_V2_BODY_MAX) {
		return FPM_HTTP_PROXY_BAD;
	}
	total = 16 + body;
	if (len < total) {
		return FPM_HTTP_PROXY_NEED_MORE;
	}
	memset(&tmp, 0, sizeof(tmp));
	tmp.length = total;
	/* LOCAL (command 0), UNSPEC and UNIX families: no client address, so the
	 * socket peer stays the client. The body is skipped, TLVs included. */
	if (command == 0 || family == 0 || family == 3) {
		*out = tmp;
		return FPM_HTTP_PROXY_DONE;
	}
	if (family == 1) {
		if (body < 12) {
			return FPM_HTTP_PROXY_BAD;
		}
		if (!inet_ntop(AF_INET, buf + 16, tmp.addr, sizeof(tmp.addr))) {
			return FPM_HTTP_PROXY_BAD;
		}
		tmp.port = (unsigned short) (((unsigned) buf[24] << 8) | buf[25]);
	} else {
		if (body < 36) {
			return FPM_HTTP_PROXY_BAD;
		}
		if (!inet_ntop(AF_INET6, buf + 16, tmp.addr, sizeof(tmp.addr))) {
			return FPM_HTTP_PROXY_BAD;
		}
		tmp.port = (unsigned short) (((unsigned) buf[48] << 8) | buf[49]);
	}
	tmp.has_source = 1;
	*out = tmp;
	return FPM_HTTP_PROXY_DONE;
}

enum fpm_http_proxy_status fpm_http_proxy_parse(const unsigned char *buf, size_t len, struct fpm_http_proxy_header_s *out)
{
	if (len == 0) {
		return FPM_HTTP_PROXY_NEED_MORE;
	}
	if (buf[0] == 'P') {
		return proxy_parse_v1(buf, len, out);
	}
	if (buf[0] == 0x0D) {
		return proxy_parse_v2(buf, len, out);
	}
	return FPM_HTTP_PROXY_BAD;
}
