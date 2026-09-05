/* fpm-ng: see fpm_http_acl.h. */

#include "fpm_config.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

#include "fpm_http_acl.h"
#include "zlog.h"

enum fpm_http_acl_family_e {
	FPM_HTTP_ACL_INET,
	FPM_HTTP_ACL_INET6
};

struct fpm_http_acl_entry_s {
	enum fpm_http_acl_family_e family;
	union {
		struct in_addr v4;
		struct in6_addr v6;
	} addr;
};

struct fpm_http_acl_s {
	unsigned count;
	struct fpm_http_acl_entry_s entries[1];	/* flexible-ish, count entries follow */
};

int fpm_http_acl_parse(const char *pool, const char *directive, const char *csv, struct fpm_http_acl_s **out) /* {{{ */
{
	char *dup, *save = NULL, *tok;
	unsigned n = 0, i;
	struct fpm_http_acl_s *acl;

	*out = NULL;

	if (!csv || !*csv) {
		return 0;
	}

	dup = strdup(csv);
	if (!dup) {
		return -1;
	}

	for (tok = dup; *tok; tok++) {
		if (*tok == ',') {
			n++;
		}
	}
	n++; /* commas + 1 = token count, upper bound (empty tokens are skipped below) */

	acl = calloc(1, sizeof(*acl) + (n ? n - 1 : 0) * sizeof(acl->entries[0]));
	if (!acl) {
		free(dup);
		return -1;
	}

	i = 0;
	for (tok = strtok_r(dup, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		while (*tok == ' ' || *tok == '\t') {
			tok++;
		}
		if (!*tok) {
			continue;
		}

		if (inet_pton(AF_INET, tok, &acl->entries[i].addr.v4) == 1) {
			acl->entries[i].family = FPM_HTTP_ACL_INET;
			i++;
		} else if (inet_pton(AF_INET6, tok, &acl->entries[i].addr.v6) == 1) {
			acl->entries[i].family = FPM_HTTP_ACL_INET6;
			i++;
		} else {
			zlog(ZLOG_ERROR, "[pool %s] %s: '%s' is not a valid IP address", pool, directive, tok);
			free(dup);
			free(acl);
			return -1;
		}
	}

	free(dup);
	acl->count = i;

	if (i == 0) {
		/* only separators/whitespace, e.g. "," or " " */
		free(acl);
		return 0;
	}

	*out = acl;
	return 0;
}
/* }}} */

void fpm_http_acl_free(struct fpm_http_acl_s *acl) /* {{{ */
{
	free(acl);
}
/* }}} */

int fpm_http_acl_check(struct fpm_http_acl_s *acl, const char *peer_addr) /* {{{ */
{
	struct in_addr v4;
	struct in6_addr v6;
	int is_v4, is_v6;
	unsigned i;

	if (!acl || !acl->count) {
		return 1;
	}
	if (!peer_addr) {
		return 0;
	}

	is_v4 = inet_pton(AF_INET, peer_addr, &v4) == 1;
	is_v6 = !is_v4 && inet_pton(AF_INET6, peer_addr, &v6) == 1;
	if (!is_v4 && !is_v6) {
		return 0;
	}

	for (i = 0; i < acl->count; i++) {
		if (is_v4 && acl->entries[i].family == FPM_HTTP_ACL_INET
				&& !memcmp(&v4, &acl->entries[i].addr.v4, sizeof(v4))) {
			return 1;
		}
		if (is_v6 && acl->entries[i].family == FPM_HTTP_ACL_INET6
				&& !memcmp(&v6, &acl->entries[i].addr.v6, sizeof(v6))) {
			return 1;
		}
#ifdef IN6_IS_ADDR_V4MAPPED
		if (is_v6 && acl->entries[i].family == FPM_HTTP_ACL_INET
				&& IN6_IS_ADDR_V4MAPPED(&v6)
				&& !memcmp(((char *)&v6) + 12, &acl->entries[i].addr.v4, 4)) {
			return 1;
		}
#endif
	}

	return 0;
}
/* }}} */
