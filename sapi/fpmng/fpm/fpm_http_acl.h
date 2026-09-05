/* fpm-ng: literal-IP allow/trust lists for the HTTP gateway.
 *
 * Same idea and same matching rules as FastCGI's listen.allowed_clients
 * (main/fastcgi.c: fcgi_set_allowed_clients()/fcgi_is_allowed()) — a plain
 * comma-separated list of literal IPv4/IPv6 addresses, no CIDR, an IPv4
 * client is also matched against an allowed IPv4-mapped IPv6 entry. Kept as
 * its own file because fpm_http.c does not otherwise need libevent's peer
 * address to become a sockaddr, and this has nothing to do with FastCGI
 * framing.
 *
 * Two different directives share this matcher: http.allowed_clients (who may
 * connect at all) and http.trusted_proxies (who may be believed about
 * X-Forwarded-*, see fpm_http_forwarded.h) -- same "list of literal IPs, deny
 * on parse error" semantics, different question being asked.
 */

#ifndef FPM_HTTP_ACL_H
#define FPM_HTTP_ACL_H 1

struct fpm_http_acl_s;

/* Parses a comma-separated list of literal IPv4/IPv6 addresses. NULL or an
 * empty string means "no restriction" and returns NULL (fpm_http_acl_check()
 * then always allows). Returns -1 on a malformed address (logs which one,
 * naming `directive` so the message points at the right config line -- e.g.
 * "http.allowed_clients" or "http.trusted_proxies" -- via `pool`); the caller
 * must refuse to start the gateway in that case, exactly like
 * listen.allowed_clients would refuse a bad address. On success writes the
 * parsed list to *out (possibly NULL for "no restriction") and returns 0. */
int fpm_http_acl_parse(const char *pool, const char *directive, const char *csv, struct fpm_http_acl_s **out);

void fpm_http_acl_free(struct fpm_http_acl_s *acl);

/* 1 if the textual address (as evhttp_connection_get_peer() gives it) is
 * allowed, 0 otherwise. acl == NULL always allows. */
int fpm_http_acl_check(struct fpm_http_acl_s *acl, const char *peer_addr);

#endif
