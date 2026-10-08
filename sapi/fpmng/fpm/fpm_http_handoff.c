/* fpm-ng: issue #661 -- gateway listeners across the reload's execvp().
 * The design and its limits are in fpm_http_handoff.h. This file holds the
 * record format, the matching rules and the ownership of each socket. */

#include "fpm_config.h"

#ifdef HAVE_FPM_HTTP

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "fpm_http_handoff.h"
#include "fpm_http_internal.h"
#include "fpm_http.h"
#include "fpm_conf.h"
#include "fpm_pool_type.h"
#include "fpm_worker_pool.h"
#include "zlog.h"

/* The listeners this master owns after begin(): inherited from the previous
 * generation, not yet taken over. fd is -1 once a socket is adopted or closed.
 * A listener is matched by its bind text and its TLS fingerprint, see
 * fpm_http_handoff_listen(). */
struct fpm_http_handed_s {
	int fd;
	char *key;
	char tls[17];
};

static struct fpm_http_handed_s *handed;
static unsigned handed_count;

void fpm_http_handoff_tls_fp(const struct fpm_worker_pool_s *wp, char out[17]) /* {{{ */
{
	const char *fields[6];
	uint64_t h = 0xcbf29ce484222325ULL; /* FNV-1a 64-bit offset basis */
	unsigned i;

	fields[0] = wp->config->http_tls_cert;
	fields[1] = wp->config->http_tls_key;
	fields[2] = wp->config->http_tls_min_version;
	fields[3] = wp->config->http_tls_sni_cert;
	fields[4] = wp->config->http_tls_verify_client;
	fields[5] = wp->config->http_tls_client_ca;

	/* A separator after each value, so that "a" + "bc" and "ab" + "c" differ. */
	for (i = 0; i < 6; i++) {
		const char *p = fields[i] ? fields[i] : "";

		for (; *p; p++) {
			h ^= (unsigned char) *p;
			h *= 0x100000001b3ULL;
		}
		h ^= 0x1f;
		h *= 0x100000001b3ULL;
	}
	h ^= wp->config->http_tls_wait_for_cert ? '1' : '0';
	h *= 0x100000001b3ULL;

	snprintf(out, 17, "%016llx", (unsigned long long) h);
}
/* }}} */

/* Adds one entry to the table. On failure the caller still owns key and fd. */
static int handed_add(int fd, char *key, const char *tls) /* {{{ */
{
	struct fpm_http_handed_s *grown = realloc(handed, (handed_count + 1) * sizeof(*handed));

	if (!grown) {
		return -1;
	}
	handed = grown;
	handed[handed_count].fd = fd;
	handed[handed_count].key = key;
	memcpy(handed[handed_count].tls, tls, sizeof(handed[handed_count].tls));
	handed_count++;
	return 0;
}
/* }}} */

/* True when some gateway in the configuration binds key, as its public
 * listen or as its http.plain_listen. Only a gateway can take a socket over,
 * so any other inherited socket is closed before a pool binds. */
static int handed_is_configured(const char *key) /* {{{ */
{
	struct fpm_worker_pool_s *wp;

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		if (fpm_pool_type_of(wp)->init_main != fpm_http_init_pool) {
			continue;
		}
		if (strcmp(key, wp->config->listen_address) == 0) {
			return 1;
		}
		if (wp->config->http_plain_listen && *wp->config->http_plain_listen &&
				strcmp(key, wp->config->http_plain_listen) == 0) {
			return 1;
		}
	}
	return 0;
}
/* }}} */

void fpm_http_handoff_begin(void) /* {{{ */
{
	const char *env = getenv(FPM_HTTP_HANDOFF_ENV);
	char *list, *save = NULL, *tok;
	unsigned i;

	if (!env) {
		return;
	}
	list = strdup(env);
	unsetenv(FPM_HTTP_HANDOFF_ENV);
	if (!list) {
		zlog(ZLOG_ERROR, "http: cannot read the listeners of the previous generation: out of memory");
		return;
	}

	for (tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *colon, *key;
		char tls[17];
		long fd;

		/* "<fd>:<16 hex digits>:<bind>". The bind text is never empty. */
		fd = strtol(tok, &colon, 10);
		if (colon == tok || *colon != ':' || fd < 0 || fd > INT_MAX || strlen(colon) < 19 ||
				colon[17] != ':' || fcntl((int) fd, F_GETFD) < 0) {
			continue;
		}
		memcpy(tls, colon + 1, 16);
		tls[16] = '\0';

		key = strdup(colon + 18);
		if (!key || handed_add((int) fd, key, tls) != 0) {
			free(key);
			close((int) fd);
		}
	}
	free(list);

	for (i = 0; i < handed_count; i++) {
		if (handed[i].fd < 0 || handed_is_configured(handed[i].key)) {
			continue;
		}
		zlog(ZLOG_NOTICE, "http: closing the listener on %s of the previous generation: no gateway uses that address now", handed[i].key);
		close(handed[i].fd);
		handed[i].fd = -1;
		free(handed[i].key);
		handed[i].key = NULL;
	}
}
/* }}} */

int fpm_http_handoff_listen(const char *pool, const char *listen_address, const char *http_address, int backlog, int reuseport, int do_listen, const char *tls_fp) /* {{{ */
{
	int adopted = -1;
	unsigned i;

	/* A socket with the same bind text but another TLS setup, or one that
	 * would only be bound and not listened on, is closed here. Keeping it
	 * would block the bind below with EADDRINUSE. */
	for (i = 0; http_address && i < handed_count; i++) {
		struct fpm_http_handed_s *h = &handed[i];

		if (h->fd < 0 || strcmp(h->key, http_address) != 0) {
			continue;
		}
		if (adopted < 0 && !reuseport && do_listen && strcmp(h->tls, tls_fp) == 0 &&
				listen(h->fd, backlog) == 0) {
			/* Inherited without FD_CLOEXEC. Set it again, as fpm_http_listen() does. */
			fcntl(h->fd, F_SETFD, fcntl(h->fd, F_GETFD) | FD_CLOEXEC);
			adopted = h->fd;
			zlog(ZLOG_NOTICE, "[pool %s] http: took over the listener on %s from the previous generation", pool, http_address);
		} else {
			close(h->fd);
		}
		h->fd = -1;
	}
	if (adopted >= 0) {
		return adopted;
	}
	return fpm_http_listen(pool, listen_address, http_address, backlog, reuseport, do_listen);
}
/* }}} */

void fpm_http_handoff_finish(void) /* {{{ */
{
	unsigned i;

	for (i = 0; i < handed_count; i++) {
		if (handed[i].fd >= 0) {
			zlog(ZLOG_WARNING, "http: closing the listener on %s of the previous generation: no gateway took it over", handed[i].key);
			close(handed[i].fd);
		}
		free(handed[i].key);
	}
	free(handed);
	handed = NULL;
	handed_count = 0;
}
/* }}} */

/* The bind text of one gateway listener that can go to the next generation,
 * or NULL. Only a TCP listener of a proxy_only gateway without reuseport
 * qualifies: a unix listener is a dup() of the pool's own socket, and a
 * reuseport gateway's children bind their own sockets. */
static char *export_key(const struct fpm_http_gateway_s *gw, int which) /* {{{ */
{
	int fd = which ? gw->plain_listen_fd : gw->listen_fd;
	const char *bind = which ? gw->plain_listen_address : gw->listen_address;
	struct sockaddr_storage ss;
	socklen_t len = sizeof(ss);
	char *key;

	if (fd < 0 || !gw->proxy_only || gw->reuseport || !bind) {
		return NULL;
	}
	if (getsockname(fd, (struct sockaddr *) &ss, &len) != 0 ||
			(ss.ss_family != AF_INET && ss.ss_family != AF_INET6)) {
		return NULL;
	}
	key = strdup(bind);
	if (key && (!*key || strchr(key, ','))) {
		free(key);
		return NULL;
	}
	return key;
}
/* }}} */

/* Appends "<sep><fd>:<tls>:<key>" to *list. Returns 0, or -1 on failure with
 * *list still valid. */
static int append_record(char **list, size_t *len, int fd, const char *key, const char *tls) /* {{{ */
{
	const char *sep = *len ? "," : "";
	int n = snprintf(NULL, 0, "%s%d:%s:%s", sep, fd, tls, key);
	char *grown;

	if (n < 0) {
		return -1;
	}
	grown = realloc(*list, *len + (size_t) n + 1);
	if (!grown) {
		return -1;
	}
	*list = grown;
	snprintf(*list + *len, (size_t) n + 1, "%s%d:%s:%s", sep, fd, tls, key);
	*len += (size_t) n;
	return 0;
}
/* }}} */

void fpm_http_handoff_export(void) /* {{{ */
{
	struct fpm_http_gateway_s *gw;
	char *list = NULL;
	size_t len = 0;
	unsigned count = 0;
	int which;

	/* Pass one builds the record list. Nothing changes yet, so a failure
	 * here leaves every listener as it was, and it closes as before. */
	for (gw = gateways; gw; gw = gw->next) {
		for (which = 0; which < 2; which++) {
			char *key = export_key(gw, which);
			int fd = which ? gw->plain_listen_fd : gw->listen_fd;

			if (!key) {
				continue;
			}
			if (append_record(&list, &len, fd, key, gw->listen_tls_fp) == 0) {
				count++;
			} else {
				zlog(ZLOG_WARNING, "[pool %s] http: the listener on %s is not handed to the next generation: out of memory", gw->pool, key);
			}
			free(key);
		}
	}
	if (!list) {
		return;
	}
	if (setenv(FPM_HTTP_HANDOFF_ENV, list, 1) != 0) {
		zlog(ZLOG_ERROR, "http: cannot hand the listeners to the next generation: setenv() failed: %s", strerror(errno));
		free(list);
		return;
	}
	free(list);

	/* Pass two marks the sockets. The same test as pass one, so it selects
	 * the same listeners. FD_CLOEXEC must go, or execvp() closes them. */
	for (gw = gateways; gw; gw = gw->next) {
		for (which = 0; which < 2; which++) {
			char *key = export_key(gw, which);
			int fd = which ? gw->plain_listen_fd : gw->listen_fd;

			if (!key) {
				continue;
			}
			free(key);
			fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) & ~FD_CLOEXEC);
			if (which) {
				gw->plain_listen_fd = -1;
			} else {
				gw->listen_fd = -1;
			}
		}
	}
	zlog(ZLOG_NOTICE, "http: handing %u gateway listener(s) to the next generation", count);
}
/* }}} */

#endif /* HAVE_FPM_HTTP */
