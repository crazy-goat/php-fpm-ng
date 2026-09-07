/* fpm-ng: X-Forwarded-For / -Proto / -Port for the HTTP gateway (fpm_http.c).
 *
 * Behind a reverse proxy, REMOTE_ADDR as seen by the gateway is the proxy's
 * address, not the client's. Trust X-Forwarded-* headers ONLY when the
 * connection comes from an address in http.trusted_proxies — otherwise any
 * client could impersonate an address in logs and application access control
 * (which often trusts REMOTE_ADDR). No directive means trust nobody, the safe
 * default.
 *
 * From X-Forwarded-For, take the first address FROM THE RIGHT that is not in
 * http.trusted_proxies. Not the first from the left: nginx's default
 * $proxy_add_x_forwarded_for appends the client address to the header the
 * client sent, so the left side is directly controlled by the client and using
 * it would allow impersonating any address DESPITE a trusted proxy. This works
 * for one proxy and for a chain.
 */

#ifndef FPM_HTTP_FORWARDED_H
#define FPM_HTTP_FORWARDED_H 1

struct fpm_http_acl_s;
struct evkeyvalq;

#define FPM_HTTP_FORWARDED_ADDR_LEN 46 /* INET6_ADDRSTRLEN */
#define FPM_HTTP_FORWARDED_PORT_LEN 6  /* "65535" + NUL */

struct fpm_http_forwarded_result_s {
	char remote_addr[FPM_HTTP_FORWARDED_ADDR_LEN]; /* [0] == '\0' -> do not override REMOTE_ADDR */
	const char *scheme;                            /* "http" or "https", never NULL */
	int https;                                     /* 1 -> HTTPS should be set to "on" */
	char server_port[FPM_HTTP_FORWARDED_PORT_LEN]; /* [0] == '\0' -> do not override SERVER_PORT */
};

/* trusted == NULL -> trust nobody, ignore all X-Forwarded-* headers, and give
 * *out clean defaults (http, no overrides). Otherwise check peer_addr (the
 * direct TCP address) against `trusted` (fpm_http_acl_check()) before reading
 * headers from `headers`. */
void fpm_http_forwarded_resolve(struct fpm_http_acl_s *trusted, const char *peer_addr,
	struct evkeyvalq *headers, struct fpm_http_forwarded_result_s *out);

#endif
