/* fpm-ng: rejestr typow poola. Patrz fpm_pool_type.h. */

#include "fpm_config.h"

#include <string.h>
#include <stdio.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_type.h"
#include "fpm_http.h"
#include "fpm_scoreboard.h"

static int fpm_pool_type_http_init(struct fpm_worker_pool_s *wp)
{
	return fpm_http_init_pool(wp);
}

/* JEDYNE miejsce, ktore trzeba dotknac, dodajac typ. */
static const struct fpm_pool_type_s fpm_pool_types[] = {
	{
		.name            = "fcgi",
		.requires_listen = 1,
		.requires_pm     = 1,
		.serves_requests = 1,
	},
	{
		.name            = "http",
		.requires_listen = 1,
		.requires_pm     = 1,
		.serves_requests = 1,
		.init_main       = fpm_pool_type_http_init,
	},
};

#define FPM_POOL_TYPE_COUNT (sizeof(fpm_pool_types) / sizeof(fpm_pool_types[0]))
#define FPM_POOL_TYPE_DEFAULT (&fpm_pool_types[0])

const struct fpm_pool_type_s *fpm_pool_type_get(const char *name)
{
	size_t i;

	if (!name || !*name) {
		return FPM_POOL_TYPE_DEFAULT;
	}

	for (i = 0; i < FPM_POOL_TYPE_COUNT; i++) {
		if (!strcmp(fpm_pool_types[i].name, name)) {
			return &fpm_pool_types[i];
		}
	}

	return NULL;
}

void fpm_pool_type_list(char *buf, size_t len)
{
	size_t i, off = 0;

	if (!len) {
		return;
	}
	buf[0] = '\0';

	for (i = 0; i < FPM_POOL_TYPE_COUNT && off + 1 < len; i++) {
		int n = snprintf(buf + off, len - off, "%s%s",
			i ? ", " : "", fpm_pool_types[i].name);
		if (n < 0 || (size_t)n >= len - off) {
			break;
		}
		off += (size_t)n;
	}
}

const struct fpm_pool_type_s *fpm_pool_type_of(struct fpm_worker_pool_s *wp)
{
	const struct fpm_pool_type_s *type = fpm_pool_type_get(wp->config->type);

	return type ? type : FPM_POOL_TYPE_DEFAULT;
}

/* Dziecko musi znac swoj pool, a przy wskrzeszaniu w petli zdarzen wskaznik na
 * niego przepada w fpm_children.c. Scoreboard jest per pool i dziecko dostaje
 * swoj w fpm_scoreboard_init_child(), wiec wystarczy dopasowanie — bez
 * dotykania fpm_children.c. Wolane raz na starcie dziecka. */
struct fpm_worker_pool_s *fpm_pool_type_current_pool(void)
{
	struct fpm_scoreboard_s *sb = fpm_scoreboard_get();
	struct fpm_worker_pool_s *wp;

	if (!sb) {
		return NULL;
	}

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		if (wp->scoreboard == sb) {
			return wp;
		}
	}

	return NULL;
}
