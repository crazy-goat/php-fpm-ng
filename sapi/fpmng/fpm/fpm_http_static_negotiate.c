/* Accept-Encoding and Cache-Control for static files. See
 * fpm_http_static_negotiate.h. */

#include "fpm_config.h"

#include "fpm_http_static_negotiate.h"

#include "fpm_conf.h"

#include <string.h>
#include <strings.h>

/* A q-value of zero means "not acceptable". Read by hand, not with strtod(),
 * so the answer does not depend on LC_NUMERIC. "0", "0.", "0.000" are zero;
 * anything with a non-zero digit, or a value this does not understand, is
 * taken as acceptable. */
static int fpm_http_static_qvalue_is_zero(const char *p, size_t n)
{
	int digit = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		if (p[i] == '0') {
			digit = 1;
		} else if (p[i] != '.') {
			return 0;
		}
	}

	return digit;
}

/* `params` points at the first ';' after a coding name, or is NULL. Only the
 * q parameter matters here. */
static int fpm_http_static_params_refuse(const char *params, const char *stop)
{
	const char *p = params;

	while (p && p < stop) {
		const char *psemi = memchr(p + 1, ';', (size_t) (stop - (p + 1)));
		const char *pend = psemi ? psemi : stop;
		const char *q = p + 1;

		while (q < pend && (*q == ' ' || *q == '\t')) {
			q++;
		}
		if (pend - q >= 2 && (*q == 'q' || *q == 'Q') && q[1] == '=') {
			const char *v_end = pend;

			while (v_end > q + 2 && (v_end[-1] == ' ' || v_end[-1] == '\t')) {
				v_end--;
			}
			return fpm_http_static_qvalue_is_zero(q + 2, (size_t) (v_end - (q + 2)));
		}
		p = psemi;
	}

	return 0;
}

int fpm_http_static_accepts(const char *accept, const char *coding)
{
	const char *p, *end;
	size_t clen = strlen(coding);

	if (!accept) {
		return 0;
	}
	for (p = accept;; p = end + 1) {
		const char *stop, *semi, *name, *name_stop;

		end = strchr(p, ',');
		stop = end ? end : p + strlen(p);
		semi = memchr(p, ';', (size_t) (stop - p));
		name = p;
		name_stop = semi ? semi : stop;
		while (name < name_stop && (*name == ' ' || *name == '\t')) {
			name++;
		}
		while (name_stop > name && (name_stop[-1] == ' ' || name_stop[-1] == '\t')) {
			name_stop--;
		}
		if ((size_t) (name_stop - name) == clen && !strncasecmp(name, coding, clen) &&
				!fpm_http_static_params_refuse(semi, stop)) {
			return 1;
		}
		if (!end) {
			return 0;
		}
	}
}

const char *fpm_http_static_cache_control_for(const struct key_value_s *rules, const char *path)
{
	const char *best = NULL;
	size_t best_len = 0;

	for (; rules; rules = rules->next) {
		size_t n = strlen(rules->key);

		if (n > best_len && strncmp(path, rules->key, n) == 0) {
			best = rules->value;
			best_len = n;
		}
	}

	return best;
}
