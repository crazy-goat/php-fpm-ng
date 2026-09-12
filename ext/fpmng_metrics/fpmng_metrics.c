/* fpmng_metrics: application metrics fed from PHP (docs/NOTES.md 3k).
 *
 * Two backends, one piece of PHP code:
 *  - under fpm-ng: series arrays in shared memory, one per worker (see
 *    php_fpmng_metrics.h), exposed on a pool's pm.metrics_path —
 *    the application serves nothing by itself;
 *  - under CLI (and every other SAPI): a process-local array; the text to
 *    expose is returned by fpm_metric_render().
 *
 * Assumptions carried over from NOTES 3k:
 *  - every function returns bool; false means a detected problem (series
 *    limit exhausted, bad name, type conflict), not decoration;
 *  - a warning in the log ONCE per topic (here: per metric), not per request;
 *  - register is optional — the type follows from the function used
 *    (inc -> counter, set -> gauge, observe -> histogram);
 *  - cardinality: a fixed series limit per worker (INI
 *    fpmng_metrics.series_limit); when exhausted we reject and return
 *    false, we never grow;
 *  - histograms exist, with no exemplars and no quantile estimation;
 *    buckets are part of the series labels (le="..."), so two workers with
 *    different bucket sets simply emit different le sets and do not bite;
 *  - under fpm-ng every series gets the automatic label pool="..." —
 *    otherwise the application's and a consumer's jobs_total would merge
 *    into one series with no trace. A user-provided "pool" label is
 *    rejected.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "php_fpmng_metrics.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ===== process state ===== */

/* Default buckets (NOTES 3k: 5 ms — 60 s). */
const double fpmng_metrics_default_buckets[FPMNG_METRICS_DEFAULT_BUCKETS_N] = {
	0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10, 30, 60
};

/* Set once by the master (inherited across fork). */
static struct fpmng_metrics_shm_s *m_shm = NULL;
static uint32_t m_slot_count = 0;
static uint32_t m_limit = 0;
static zend_bool m_mode_shm = 0;		/* 0 = local backend (CLI) */

/* The process's own slot: shm — a pointer to the worker's array; locally —
 * an array on the heap. NULL in shm mode = "I was not attached" (master,
 * gateway), and then every operation returns false. */
static struct fpmng_metrics_slot_s *m_slot = NULL;

/* The pool label value (shm mode), a local copy — the scoreboard may be
 * freed later by the child's hygiene. */
static char m_pool[33];

/* "Once per topic" warnings: a list of topics that already got an
 * E_WARNING. A topic is, e.g., the full metric name. A plain array — the
 * warning limit is iron; past it we stay silent (needless noise is worse). */
static char m_warned[64][80];
static uint32_t m_warned_n = 0;

ZEND_BEGIN_MODULE_GLOBALS(fpmng_metrics)
	zend_long series_limit;
ZEND_END_MODULE_GLOBALS(fpmng_metrics)

ZEND_DECLARE_MODULE_GLOBALS(fpmng_metrics)

#define FPMNG_METRICS_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(fpmng_metrics, v)

PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("fpmng_metrics.series_limit", "256", PHP_INI_SYSTEM,
		OnUpdateLong, series_limit, zend_fpmng_metrics_globals, fpmng_metrics_globals)
PHP_INI_END()

/* ===== array geometry ===== */

static size_t entry_off(void)
{
	return (sizeof(struct fpmng_metrics_slot_s) + 7u) & ~(size_t) 7u;
}

static struct fpmng_metrics_entry_s *slot_entry(struct fpmng_metrics_slot_s *s, uint32_t i)
{
	return (struct fpmng_metrics_entry_s *) ((char *) s + entry_off()
		+ (size_t) i * sizeof(struct fpmng_metrics_entry_s));
}

static size_t slot_size(uint32_t limit)
{
	return entry_off() + (size_t) limit * sizeof(struct fpmng_metrics_entry_s);
}

size_t fpmng_metrics_shm_size(uint32_t slots, uint32_t limit)
{
	if (!slots || !limit) {
		return 0;
	}
	return ((sizeof(struct fpmng_metrics_shm_s) + 7u) & ~(size_t) 7u)
		+ (size_t) slots * slot_size(limit);
}

static struct fpmng_metrics_slot_s *shm_slot(struct fpmng_metrics_shm_s *shm, uint32_t i)
{
	size_t hdr = (sizeof(struct fpmng_metrics_shm_s) + 7u) & ~(size_t) 7u;

	return (struct fpmng_metrics_slot_s *) ((char *) shm + hdr + (size_t) i * slot_size(shm->limit));
}

/* ===== C-side API (see php_fpmng_metrics.h) ===== */

void fpmng_metrics_shm_init(void *mem, size_t size, uint32_t slots, uint32_t limit)
{
	struct fpmng_metrics_shm_s *shm = mem;

	if (!mem || !slots || !limit || size < fpmng_metrics_shm_size(slots, limit)) {
		return;
	}
	/* fpm_shm_alloc() returns a fresh MAP_ANONYMOUS mapping, which the kernel
	 * zero-fills lazily. Do not eagerly fault in the whole per-worker region:
	 * pm.max_children=12800 and series_limit=256 reserve about 3.2 GiB. */
	shm->magic = FPMNG_METRICS_MAGIC;
	shm->slots = slots;
	shm->limit = limit;

	m_shm = shm;
	m_slot_count = slots;
	m_limit = limit;
	m_mode_shm = 1;
	m_slot = NULL;			/* the master does not write metrics */
}

void fpmng_metrics_child_attach(uint32_t slot_index, const char *pool_name)
{
	if (!m_mode_shm || !m_shm || slot_index >= m_shm->slots) {
		return;
	}
	m_slot = shm_slot(m_shm, slot_index);
	m_pool[0] = '\0';
	if (pool_name) {
		strlcpy(m_pool, pool_name, sizeof(m_pool));
	}
}

uint32_t fpmng_metrics_series_limit(void)
{
	zend_long v = FPMNG_METRICS_G(series_limit);

	if (v < 1) {
		return 1;
	}
	if (v > 100000) {
		return 100000;
	}
	return (uint32_t) v;
}

/* ===== helpers ===== */

/* Warn once per topic. Returns 1 if the warning was fresh (first time),
 * 0 if the topic was already seen. */
static int warn_once(const char *topic, const char *fmt, const char *arg)
{
	uint32_t i;

	if (!topic || !*topic) {
		return 0;
	}
	for (i = 0; i < m_warned_n; i++) {
		if (!strcmp(m_warned[i], topic)) {
			return 0;
		}
	}
	if (m_warned_n < sizeof(m_warned) / sizeof(m_warned[0])) {
		strlcpy(m_warned[m_warned_n++], topic, sizeof(m_warned[0]));
	}
	if (arg) {
		php_error(E_WARNING, "fpmng_metrics: %s: %s", fmt, arg);
	} else {
		php_error(E_WARNING, "fpmng_metrics: %s", fmt);
	}
	return 1;
}

static int valid_metric_name(const char *s, size_t len)
{
	size_t i;

	if (!len || len >= FPMNG_METRICS_NAME_MAX) {
		return 0;
	}
	if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')
		|| s[0] == '_' || s[0] == ':')) {
		return 0;
	}
	for (i = 1; i < len; i++) {
		char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9') || c == '_' || c == ':')) {
			return 0;
		}
	}
	return 1;
}

static int valid_label_name(const char *s, size_t len)
{
	size_t i;

	if (!len || len >= FPMNG_METRICS_LBLNAME_MAX) {
		return 0;
	}
	if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') || s[0] == '_')) {
		return 0;
	}
	for (i = 1; i < len; i++) {
		char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9') || c == '_')) {
			return 0;
		}
	}
	return 1;
}

/* Builds the series key: name{k1="v1",k2="v2"} (labels sorted by name —
 * a stable key regardless of array order; in shm mode the pool label always
 * comes first). Returns 0/-1. */
static int build_key(char *out, size_t outsz, const char *name,
	const char *const *lnames, const char *const *lvals, uint32_t nlabels)
{
	uint32_t order[FPMNG_METRICS_LBL_MAX];
	uint32_t i, j;
	size_t pos;
	int any;

	if (nlabels > FPMNG_METRICS_LBL_MAX) {
		return -1;
	}

	/* insertion sort — nlabels <= 8 */
	for (i = 0; i < nlabels; i++) {
		order[i] = i;
		for (j = i; j > 0; j--) {
			if (strcmp(lnames[order[j - 1]], lnames[order[j]]) <= 0) {
				break;
			}
			order[j - 1] ^= order[j]; order[j] ^= order[j - 1]; order[j - 1] ^= order[j];
		}
	}

	pos = strlen(name);
	if (pos >= outsz) {
		return -1;
	}
	memcpy(out, name, pos);

	any = (m_mode_shm && m_pool[0]) || nlabels > 0;
	if (any) {
		out[pos++] = '{';
	}
	if (m_mode_shm && m_pool[0]) {
		int w = snprintf(out + pos, outsz - pos, "pool=\"%s\"", m_pool);
		if (w < 0 || (size_t) w >= outsz - pos) {
			return -1;
		}
		pos += (size_t) w;
	}
	for (i = 0; i < nlabels; i++) {
		const char *v = lvals[order[i]];
		const char *p;
		size_t vlen = strlen(v);

		if (vlen > FPMNG_METRICS_LBLVAL_MAX) {
			return -1;
		}
		if (i > 0 || (m_mode_shm && m_pool[0])) {
			if (pos + 1 >= outsz) {
				return -1;
			}
			out[pos++] = ',';
		}
		{
			int w = snprintf(out + pos, outsz - pos, "%s=\"", lnames[order[i]]);
			if (w < 0 || (size_t) w >= outsz - pos) {
				return -1;
			}
			pos += (size_t) w;
		}
		for (p = v; *p; p++) {	/* escape: \ " \n */
			char plain[2] = { *p, '\0' };
			char esc[3] = { '\\', *p, '\0' };

			if (*p == '\n') {
				esc[1] = 'n';
			} else if (*p != '\\' && *p != '"') {
				esc[1] = '\0';
			}
			if (!esc[1]) {
				if (pos + 1 >= outsz) {
					return -1;
				}
				out[pos++] = plain[0];
			} else {
				if (pos + 2 >= outsz) {
					return -1;
				}
				out[pos++] = esc[0];
				out[pos++] = esc[1];
			}
		}
		if (pos + 1 >= outsz) {
			return -1;
		}
		out[pos++] = '"';
	}
	if (any) {
		if (pos + 1 >= outsz) {
			return -1;
		}
		out[pos++] = '}';
	}
	out[pos] = '\0';
	return 0;
}

/* Ensures the array in local mode (CLI). Returns 0/-1. */
static int ensure_local(void)
{
	if (m_mode_shm) {
		return m_slot ? 0 : -1;	/* shm mode without attach: master/gateway */
	}
	if (m_slot) {
		return 0;
	}
	m_limit = fpmng_metrics_series_limit();
	m_slot_count = 1;
	m_slot = calloc(1, slot_size(m_limit));
	return m_slot ? 0 : -1;
}

/* Finds a series in the OWN slot by key. */
static struct fpmng_metrics_entry_s *find_series(const char *key)
{
	uint32_t i;

	for (i = 0; i < m_slot->used; i++) {
		struct fpmng_metrics_entry_s *e = slot_entry(m_slot, i);
		if (e->in_use && !strcmp(e->key, key)) {
			return e;
		}
	}
	return NULL;
}

/* A metric definition (register) in the own slot: an entry whose key equals
 * the bare name. NULL = none. */
static struct fpmng_metrics_entry_s *find_definition(const char *name)
{
	uint32_t i;

	for (i = 0; i < m_slot->used; i++) {
		struct fpmng_metrics_entry_s *e = slot_entry(m_slot, i);
		if (e->in_use && !strcmp(e->key, name)) {
			return e;
		}
	}
	return NULL;
}

/* A new series in the own slot; NULL when the limit is exhausted (with a
 * once-per-metric warning that names it — without that the operator would
 * not find the culprit, NOTES 3k). */
static struct fpmng_metrics_entry_s *alloc_series(const char *name)
{
	struct fpmng_metrics_entry_s *e;

	if (m_slot->used >= m_limit) {
		char topic[80];

		snprintf(topic, sizeof(topic), "limit:%s", name);
		warn_once(topic,
			"series limit exhausted (fpmng_metrics.series_limit), rejecting new series for metric", name);
		return NULL;
	}
	e = slot_entry(m_slot, m_slot->used++);
	memset(e, 0, sizeof(*e));
	e->in_use = 1;
	return e;
}

static const char *type_name(uint8_t t)
{
	switch (t) {
		case FPMNG_METRIC_COUNTER:    return "counter";
		case FPMNG_METRIC_GAUGE_SUM:  return "gauge";
		case FPMNG_METRIC_GAUGE_MAX:  return "gauge";
		case FPMNG_METRIC_HISTOGRAM:  return "histogram";
	}
	return "untyped";
}

/* ===== operations ===== */

/* Prepares a series for an operation of type `type`. Returns NULL on error
 * (already warned). nbuckets/buckets only for the histogram. */
static struct fpmng_metrics_entry_s *op_series(const char *name, const char *key,
	uint8_t type, const double *buckets, uint16_t nbuckets)
{
	struct fpmng_metrics_entry_s *e = find_series(key);
	struct fpmng_metrics_entry_s *def;

	if (e) {
		if (e->type != type) {
			char topic[80];

			snprintf(topic, sizeof(topic), "type:%s", key);
			warn_once(topic, "metric type conflict, operation rejected for series", key);
			return NULL;
		}
		return e;
	}

	e = alloc_series(name);
	if (!e) {
		return NULL;
	}
	strlcpy(e->key, key, sizeof(e->key));
	e->type = type;

	/* Buckets: from the definition (register) of this name, if present;
	 * otherwise the defaults. The definition lives in the own slot — a
	 * register called in the same process suffices, because it usually
	 * runs at the start of every request/job. */
	if (type == FPMNG_METRIC_HISTOGRAM) {
		def = find_definition(name);
		if (def && def->type == FPMNG_METRIC_HISTOGRAM && def->bucket_count) {
			nbuckets = def->bucket_count;
			buckets = def->buckets;
		} else if (!nbuckets) {
			nbuckets = FPMNG_METRICS_DEFAULT_BUCKETS_N;
			buckets = fpmng_metrics_default_buckets;
		}
		e->bucket_count = nbuckets > FPMNG_METRICS_BUCKETS_MAX ? FPMNG_METRICS_BUCKETS_MAX : nbuckets;
		memcpy(e->buckets, buckets, e->bucket_count * sizeof(double));
	}
	return e;
}

/* ===== render (C API, no ZEND_API — also called from the operator endpoint) ===== */

struct agg_s {
	char *key;
	char *name;			/* key without labels, for HELP/TYPE */
	char help[FPMNG_METRICS_HELP_MAX];
	uint8_t type;
	double buckets[FPMNG_METRICS_BUCKETS_MAX];
	uint16_t nbuckets;
	double v[FPMNG_METRICS_BUCKETS_MAX + 2];	/* jak w entry: counts..., sum, count */
	double value;			/* counter/gauge: sum or max */
	int have_type_conflict;
};

/* A metric's metadata (register): an entry with the bare key, no labels. In
 * shm mode this is NOT a series (every series gets pool="...", so a bare
 * key can never collide with a key from inc/set/observe) — we keep only
 * HELP/TYPE/buckets from it. In local mode (CLI, no pool label) the bare
 * key IS a series, and the value too. */
struct meta_s {
	char *name;
	char help[FPMNG_METRICS_HELP_MAX];
	uint8_t type;
	double buckets[FPMNG_METRICS_BUCKETS_MAX];
	uint16_t nbuckets;
};

struct rbuf_s {
	char *d;
	size_t len, cap;
};

static void rbuf_add(struct rbuf_s *b, const char *s, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 1024;
		char *d;
		while (cap < b->len + n + 1) {
			cap *= 2;
		}
		d = realloc(b->d, cap);
		if (!d) {
			return;
		}
		b->d = d;
		b->cap = cap;
	}
	memcpy(b->d + b->len, s, n);
	b->len += n;
	b->d[b->len] = '\0';
}

static void rbuf_addf(struct rbuf_s *b, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n > 0) {
		rbuf_add(b, tmp, (size_t) n >= sizeof(tmp) ? sizeof(tmp) - 1 : (size_t) n);
	}
}

/* metric name = the key up to the first '{' or the whole key */
static char *metric_name_of(const char *key)
{
	const char *brace = strchr(key, '{');
	size_t n = brace ? (size_t) (brace - key) : strlen(key);
	char *s = malloc(n + 1);

	if (!s) {
		return NULL;
	}
	memcpy(s, key, n);
	s[n] = '\0';
	return s;
}

static int agg_cmp(const void *a, const void *b)
{
	const struct agg_s *const *x = a;
	const struct agg_s *const *y = b;

	return strcmp((*x)->key, (*y)->key);
}

static double fmt_value(double v)
{
	return v;
}

/* Emits the series <base><suffix>{<inner>,le="<le>"} — Prometheus requires
 * the _bucket/_sum/_count suffix BEFORE the label braces, while the series
 * key in the store looks like name{pool="..",k="v"}. le = a ready label
 * literal ("0.5", "+Inf") or NULL for _sum/_count. */
static void emit_hist_series(struct rbuf_s *b, const char *key,
	const char *suffix, const char *le_literal, double value)
{
	const char *brace = strchr(key, '{');
	char head[FPMNG_METRICS_KEY_MAX + 64];

	if (!brace) {
		if (le_literal) {
			snprintf(head, sizeof(head), "%s%s{le=\"%s\"}", key, suffix, le_literal);
		} else {
			snprintf(head, sizeof(head), "%s%s", key, suffix);
		}
	} else {
		size_t base_len = (size_t) (brace - key);
		size_t inner_len = strlen(brace + 1);	/* without '}' */

		if (inner_len > 1) {	/* there are labels inside */
			if (le_literal) {
				snprintf(head, sizeof(head), "%.*s%s{%.*s,le=\"%s\"}",
					(int) base_len, key, suffix, (int) (inner_len - 1), brace + 1, le_literal);
			} else {
				snprintf(head, sizeof(head), "%.*s%s{%.*s}",
					(int) base_len, key, suffix, (int) (inner_len - 1), brace + 1);
			}
		} else {
			if (le_literal) {
				snprintf(head, sizeof(head), "%.*s%s{le=\"%s\"}", (int) base_len, key, suffix, le_literal);
			} else {
				snprintf(head, sizeof(head), "%.*s%s", (int) base_len, key, suffix);
			}
		}
	}
	rbuf_addf(b, "%s %.15g\n", head, fmt_value(value));
}

/* Bucket boundary literal: the shortest representation without losing meaning */
static void bucket_literal(char *out, size_t outsz, double le)
{
	snprintf(out, outsz, "%.15g", le);
}

int fpmng_metrics_render_range(uint32_t first_slot, uint32_t slot_count, char **out, size_t *out_len)
{
	struct rbuf_s b = {0};
	struct agg_s **aggs = NULL;
	size_t naggs = 0, cap = 0;
	uint32_t si, ei;
	struct fpmng_metrics_shm_s *shm = m_shm;
	uint32_t first, end;
	uint32_t limit;
	int is_local = 0;
	size_t i;
	char **names = NULL;
	size_t nnames = 0;

	*out = NULL;
	*out_len = 0;

	if (m_mode_shm) {
		if (!shm || shm->magic != FPMNG_METRICS_MAGIC) {
			return -1;
		}
		/* The caller asks for a half-open slot range, clamped here rather than
		 * validated: a pool whose slots were never allocated (metrics shm
		 * failed, or the pool has no children) must render an empty page, not
		 * an error, because the endpoint being up is independent of anything
		 * having been written to it. */
		first = first_slot < shm->slots ? first_slot : shm->slots;
		end = slot_count > shm->slots - first ? shm->slots : first + slot_count;
		limit = shm->limit;
	} else {
		/* CLI: render of what is in the process; an empty result is fine */
		if (!m_slot) {
			*out = malloc(1);
			if (*out) {
				(*out)[0] = '\0';
			}
			*out_len = 0;
			return 0;
		}
		/* CLI has exactly one slot and no pools to divide it between, so the
		 * range is ignored rather than applied to a process-local array whose
		 * index means nothing. */
		first = 0;
		end = 1;
		limit = m_limit;
		is_local = 1;
	}

	/* Definitions (register) first: in shm mode a bare key is metadata,
	 * not a series. In local mode a bare key is a series (inc without
	 * labels produces exactly that key), so we do not split it out there. */
	struct meta_s **metas = NULL;
	size_t nmetas = 0, mcap = 0;

	if (m_mode_shm) {
		for (si = first; si < end; si++) {
			struct fpmng_metrics_slot_s *slot = shm_slot(shm, si);

			for (ei = 0; ei < slot->used && ei < limit; ei++) {
				struct fpmng_metrics_entry_s *e = slot_entry(slot, ei);
				struct meta_s *m = NULL;
				size_t j;

				if (!e->in_use || strchr(e->key, '{')) {
					continue;
				}
				for (j = 0; j < nmetas; j++) {
					if (!strcmp(metas[j]->name, e->key)) {
						m = metas[j];
						break;
					}
				}
				if (!m) {
					if (nmetas == mcap) {
						mcap = mcap ? mcap * 2 : 16;
						/* array of pointers — sizeof(void *) is intentional */
						struct meta_s **nm = realloc(metas, mcap * sizeof(void *));
						if (!nm) {
							goto fail;
						}
						metas = nm;
					}
					m = calloc(1, sizeof(*m));
					if (!m) {
						goto fail;
					}
					m->name = strdup(e->key);
					if (!m->name) {
						free(m);
						goto fail;
					}
					metas[nmetas++] = m;
				}
				if (e->help[0] && !m->help[0]) {
					strlcpy(m->help, e->help, sizeof(m->help));
				}
				if (!m->type) {
					m->type = e->type;
				}
				if (e->type == FPMNG_METRIC_HISTOGRAM && e->bucket_count > m->nbuckets) {
					memcpy(m->buckets, e->buckets, e->bucket_count * sizeof(double));
					m->nbuckets = e->bucket_count;
				}
			}
		}
	}

	/* aggregation: by full key (with labels) */
	for (si = first; si < end; si++) {
		struct fpmng_metrics_slot_s *slot = is_local
			? m_slot
			: shm_slot(shm, si);

		for (ei = 0; ei < slot->used && ei < limit; ei++) {
			struct fpmng_metrics_entry_s *e = slot_entry(slot, ei);
			struct agg_s *a = NULL;

			if (!e->in_use) {
				continue;
			}
			if (m_mode_shm && !strchr(e->key, '{')) {
				/* definition (register) — metadata collected above */
				continue;
			}
			for (i = 0; i < naggs; i++) {
				if (!strcmp(aggs[i]->key, e->key)) {
					a = aggs[i];
					break;
				}
			}
			if (!a) {
				if (naggs == cap) {
					cap = cap ? cap * 2 : 64;
					/* array of pointers — sizeof(void *) is intentional */
					struct agg_s **na = realloc(aggs, cap * sizeof(void *));
					if (!na) {
						goto fail;
					}
					aggs = na;
				}
				a = calloc(1, sizeof(*a));
				if (!a) {
					goto fail;
				}
				a->key = strdup(e->key);
				a->name = metric_name_of(e->key);
				if (!a->key || !a->name) {
					free(a->key);
					free(a->name);
					free(a);
					goto fail;
				}
				a->type = e->type;
				aggs[naggs++] = a;
			}

			/* metadata from the definition / first-wins */
			if (e->help[0] && !a->help[0]) {
				strlcpy(a->help, e->help, sizeof(a->help));
			}
			if (e->type != a->type) {
				/* type conflict between slots (different code in different
				 * workers) — we do not merge values; they are emitted
				 * separately below, by the first type; see below. */
				a->have_type_conflict = 1;
			}
			if (e->type == FPMNG_METRIC_HISTOGRAM
					&& e->bucket_count > a->nbuckets
					&& a->type == FPMNG_METRIC_HISTOGRAM) {
				/* remember the largest known bucket set, so we know for
				 * which le values to emit the zero counts */
				memcpy(a->buckets, e->buckets, e->bucket_count * sizeof(double));
				a->nbuckets = e->bucket_count;
			}

			switch (e->type) {
				case FPMNG_METRIC_COUNTER:
				case FPMNG_METRIC_GAUGE_SUM:
					a->value += e->v[0];
					break;
				case FPMNG_METRIC_GAUGE_MAX:
					if (e->v[0] > a->value || a->v[FPMNG_METRICS_BUCKETS_MAX + 1] == 0) {
						/* first-time comparison: we use count as the
						 * "value already seen" marker */
						a->value = e->v[0];
					}
					a->v[FPMNG_METRICS_BUCKETS_MAX + 1] = 1;
					break;
				case FPMNG_METRIC_HISTOGRAM: {
					uint16_t k;
					/* per-bucket counts: the histogram has le as part
					 * of the output key, so sum over identical
					 * boundaries; counts in a->v[0..] follow the
					 * INCOMING series' bucket_count */
					for (k = 0; k < e->bucket_count; k++) {
						/* locate le in a->buckets */
						uint16_t m;
						for (m = 0; m < a->nbuckets; m++) {
							if (a->buckets[m] == e->buckets[k]) {
								break;
							}
						}
						if (m == a->nbuckets) {
							/* boundary unknown to a->buckets — reached only
							 * when emitting via the sorted aggregation;
							 * skip it for now (rare, only a bucket
							 * conflict between workers) */
							continue;
						}
						a->v[m] += e->v[k];
					}
					a->v[FPMNG_METRICS_BUCKETS_MAX] += e->v[FPMNG_METRICS_BUCKETS_MAX];
					a->v[FPMNG_METRICS_BUCKETS_MAX + 1] += e->v[FPMNG_METRICS_BUCKETS_MAX + 1];
					break;
				}
			}
		}
	}

	if (!naggs) {
		*out = malloc(1);
		if (*out) {
			(*out)[0] = '\0';
		}
		*out_len = 0;
		goto done;
	}

	/* stable output: series sorted by key — series of the same metric
	 * (same prefix before '{') land next to each other */
	{
		/* array of pointers — sizeof(void *) is intentional */
		struct agg_s **sorted = malloc(naggs * sizeof(void *));

		if (!sorted) {
			goto fail;
		}
		memcpy(sorted, aggs, naggs * sizeof(void *));
		qsort(sorted, naggs, sizeof(void *), agg_cmp);

		/* HELP/TYPE once per metric: at the first series of that name */
		names = malloc(naggs * sizeof(void *));
		if (!names) {
			free(sorted);
			goto fail;
		}
		for (i = 0; i < naggs; i++) {
			const struct agg_s *a = sorted[i];
			size_t j;
			int seen = 0;

			for (j = 0; j < nnames; j++) {
				if (!strcmp(names[j], a->name)) {
					seen = 1;
					break;
				}
			}
			if (!seen) {
				const struct meta_s *meta = NULL;

				names[nnames++] = a->name;
				for (size_t q = 0; q < nmetas; q++) {
					if (!strcmp(metas[q]->name, a->name)) {
						meta = metas[q];
						break;
					}
				}
				if ((meta && meta->help[0]) || (!meta && a->help[0])) {
					rbuf_addf(&b, "# HELP %s %s\n", a->name, meta ? meta->help : a->help);
				}
				rbuf_addf(&b, "# TYPE %s %s\n", a->name, type_name(meta ? meta->type : a->type));
			}

			if (a->type == FPMNG_METRIC_HISTOGRAM) {
				uint16_t m, n;
				/* bucket boundaries, sorted indirectly through idx[] --
				 * a->buckets[] itself is shared memory and is not reordered */
				uint16_t idx[FPMNG_METRICS_BUCKETS_MAX];

				for (m = 0; m < a->nbuckets; m++) {
					idx[m] = m;
					for (uint16_t q = m; q > 0; q--) {
						if (a->buckets[idx[q - 1]] <= a->buckets[idx[q]]) {
							break;
						}
						uint16_t t = idx[q - 1]; idx[q - 1] = idx[q]; idx[q] = t;
					}
				}
				for (m = 0; m < a->nbuckets; m++) {
					char lelit[32];

					n = idx[m];
					bucket_literal(lelit, sizeof(lelit), a->buckets[n]);
					emit_hist_series(&b, a->key, "_bucket", lelit, a->v[n]);
				}
				emit_hist_series(&b, a->key, "_bucket", "+Inf", a->v[FPMNG_METRICS_BUCKETS_MAX + 1]);
				emit_hist_series(&b, a->key, "_sum", NULL, a->v[FPMNG_METRICS_BUCKETS_MAX]);
				emit_hist_series(&b, a->key, "_count", NULL, a->v[FPMNG_METRICS_BUCKETS_MAX + 1]);
			} else {
				rbuf_addf(&b, "%s %.15g\n", a->key, fmt_value(a->value));
			}
		}
		free(sorted);
	}

	*out = b.d ? b.d : malloc(1);
	if (!*out) {
		goto fail;
	}
	if (!b.d) {
		(*out)[0] = '\0';
	}
	*out_len = b.len;
	goto done;

fail:
	free(b.d);
	*out = NULL;
	*out_len = 0;
done:
	for (i = 0; i < naggs; i++) {
		free(aggs[i]->key);
		free(aggs[i]->name);
		free(aggs[i]);
	}
	free(aggs);
	free(names);
	for (i = 0; i < nmetas; i++) {
		free(metas[i]->name);
		free(metas[i]);
	}
	free(metas);
	return *out ? 0 : (naggs ? -1 : 0);
}

int fpmng_metrics_render_text(char **out, size_t *out_len)
{
	return fpmng_metrics_render_range(0, UINT32_MAX, out, out_len);
}

/* ===== PHP functions ===== */

/* label arguments: a string => string HashTable. Splits into parallel
 * name/value arrays. Returns the label count or -1. */
static int parse_labels(zval *labels, const char *const **lnames_p,
	const char *const **lvals_p, const char *name)
{
	static const char *lnames[FPMNG_METRICS_LBL_MAX];
	static const char *lvals[FPMNG_METRICS_LBL_MAX];
	zend_string *k;
	zval *v;
	uint32_t n = 0;

	(void) name;
	*lnames_p = lnames;
	*lvals_p = lvals;

	if (!labels || Z_TYPE_P(labels) == IS_NULL) {
		return 0;
	}
	if (Z_TYPE_P(labels) != IS_ARRAY) {
		return -1;
	}
	ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(labels), k, v) {
		if (!k || Z_TYPE_P(v) != IS_STRING || n >= FPMNG_METRICS_LBL_MAX) {
			return -1;
		}
		if (!valid_label_name(ZSTR_VAL(k), ZSTR_LEN(k))) {
			return -1;
		}
		if (!strcmp(ZSTR_VAL(k), "pool")) {
			/* reserved — added by fpm-ng, see the comment at the top of the file */
			warn_once("label:pool", "label 'pool' is reserved (added automatically by fpm-ng), rejected", name);
			return -1;
		}
		if (Z_STRLEN_P(v) > FPMNG_METRICS_LBLVAL_MAX) {
			return -1;
		}
		lnames[n] = ZSTR_VAL(k);
		lvals[n] = Z_STRVAL_P(v);
		n++;
	} ZEND_HASH_FOREACH_END();
	return (int) n;
}

PHP_FUNCTION(fpm_metric_register)
{
	char *name, *type;
	char *help = NULL;
	size_t name_len, type_len, help_len = 0;
	HashTable *buckets_ht = NULL;
	double buckets[FPMNG_METRICS_BUCKETS_MAX];
	uint16_t nbuckets = 0;
	uint8_t t;
	struct fpmng_metrics_entry_s *e;

	ZEND_PARSE_PARAMETERS_START(2, 4)
		Z_PARAM_STRING(name, name_len)
		Z_PARAM_STRING(type, type_len)
		Z_PARAM_OPTIONAL
		Z_PARAM_STRING(help, help_len)
		Z_PARAM_ARRAY_HT(buckets_ht)
	ZEND_PARSE_PARAMETERS_END();

	if (!valid_metric_name(name, name_len)) {
		warn_once(name, "invalid metric name rejected", name);
		RETURN_FALSE;
	}
	if (!strcmp(type, "counter")) {
		t = FPMNG_METRIC_COUNTER;
	} else if (!strcmp(type, "gauge")) {
		t = FPMNG_METRIC_GAUGE_SUM;
	} else if (!strcmp(type, "gauge_max")) {
		t = FPMNG_METRIC_GAUGE_MAX;
	} else if (!strcmp(type, "histogram")) {
		t = FPMNG_METRIC_HISTOGRAM;
	} else {
		warn_once(type, "unknown metric type rejected", type);
		RETURN_FALSE;
	}
	if (buckets_ht) {
		zval *bv;
		ZEND_HASH_FOREACH_VAL(buckets_ht, bv) {
			if (nbuckets >= FPMNG_METRICS_BUCKETS_MAX) {
				warn_once("buckets", "too many buckets (max 32) rejected", name);
				RETURN_FALSE;
			}
			buckets[nbuckets++] = zval_get_double(bv);
		} ZEND_HASH_FOREACH_END();
		/* unsorted buckets are a valid set, but the le order on the
		 * output is sorted at render time anyway */
	}

	if (ensure_local()) {
		RETURN_FALSE;
	}

	e = find_definition(name);
	if (!e) {
		e = alloc_series(name);
		if (!e) {
			RETURN_FALSE;
		}
		strlcpy(e->key, name, sizeof(e->key));
		e->type = t;
	}
	if (e->type != t) {
		char topic[80];
		snprintf(topic, sizeof(topic), "retype:%s", name);
		warn_once(topic, "cannot change metric type for", name);
		RETURN_FALSE;
	}
	if (help_len && help_len < FPMNG_METRICS_HELP_MAX) {
		strlcpy(e->help, help, sizeof(e->help));
	}
	if (t == FPMNG_METRIC_HISTOGRAM && nbuckets) {
		/* changing buckets after observations would change the meaning of
		 * the counts; we allow it only while the definition series is empty */
		if (e->v[FPMNG_METRICS_BUCKETS_MAX + 1] != 0 && e->bucket_count != nbuckets) {
			char topic[80];
			snprintf(topic, sizeof(topic), "rebucket:%s", name);
			warn_once(topic, "cannot change histogram buckets after observations for", name);
			RETURN_FALSE;
		}
		memcpy(e->buckets, buckets, nbuckets * sizeof(double));
		e->bucket_count = nbuckets;
	}
	RETURN_TRUE;
}

PHP_FUNCTION(fpm_metric_inc)
{
	char *name;
	size_t name_len;
	double by = 1.0;
	zval *labels = NULL;
	const char *const *ln, *const *lv;
	int nlabels;
	char key[FPMNG_METRICS_KEY_MAX];
	struct fpmng_metrics_entry_s *e;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_STRING(name, name_len)
		Z_PARAM_OPTIONAL
		Z_PARAM_DOUBLE(by)
		Z_PARAM_ARRAY_OR_NULL(labels)
	ZEND_PARSE_PARAMETERS_END();

	if (!valid_metric_name(name, name_len)) {
		warn_once(name, "invalid metric name rejected", name);
		RETURN_FALSE;
	}
	nlabels = parse_labels(labels, &ln, &lv, name);
	if (nlabels < 0 || build_key(key, sizeof(key), name, ln, lv, (uint32_t) nlabels)) {
		RETURN_FALSE;
	}
	if (ensure_local()) {
		RETURN_FALSE;
	}
	e = op_series(name, key, FPMNG_METRIC_COUNTER, NULL, 0);
	if (!e) {
		RETURN_FALSE;
	}
	e->v[0] += by;
	RETURN_TRUE;
}

PHP_FUNCTION(fpm_metric_set)
{
	char *name;
	size_t name_len;
	double value;
	zval *labels = NULL;
	const char *const *ln, *const *lv;
	int nlabels;
	char key[FPMNG_METRICS_KEY_MAX];
	struct fpmng_metrics_entry_s *e;
	uint8_t t;

	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_STRING(name, name_len)
		Z_PARAM_DOUBLE(value)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY_OR_NULL(labels)
	ZEND_PARSE_PARAMETERS_END();

	if (!valid_metric_name(name, name_len)) {
		warn_once(name, "invalid metric name rejected", name);
		RETURN_FALSE;
	}
	nlabels = parse_labels(labels, &ln, &lv, name);
	if (nlabels < 0 || build_key(key, sizeof(key), name, ln, lv, (uint32_t) nlabels)) {
		RETURN_FALSE;
	}
	if (ensure_local()) {
		RETURN_FALSE;
	}
	/* set works for both gauge (sum) and gauge_max (maximum) */
	e = find_series(key);
	t = e ? e->type : FPMNG_METRIC_GAUGE_SUM;
	if (t != FPMNG_METRIC_GAUGE_SUM && t != FPMNG_METRIC_GAUGE_MAX) {
		char topic[80];
		snprintf(topic, sizeof(topic), "type:%s", key);
		warn_once(topic, "metric type conflict, operation rejected for series", key);
		RETURN_FALSE;
	}
	if (!e) {
		/* the definition may declare gauge_max */
		struct fpmng_metrics_entry_s *def = find_definition(name);
		if (def && def->type == FPMNG_METRIC_GAUGE_MAX) {
			t = FPMNG_METRIC_GAUGE_MAX;
		}
		e = op_series(name, key, t, NULL, 0);
		if (!e) {
			RETURN_FALSE;
		}
	}
	e->v[0] = value;
	RETURN_TRUE;
}

PHP_FUNCTION(fpm_metric_observe)
{
	char *name;
	size_t name_len;
	double value;
	zval *labels = NULL;
	const char *const *ln, *const *lv;
	int nlabels;
	char key[FPMNG_METRICS_KEY_MAX];
	struct fpmng_metrics_entry_s *e;
	uint16_t k;

	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_STRING(name, name_len)
		Z_PARAM_DOUBLE(value)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY_OR_NULL(labels)
	ZEND_PARSE_PARAMETERS_END();

	if (!valid_metric_name(name, name_len)) {
		warn_once(name, "invalid metric name rejected", name);
		RETURN_FALSE;
	}
	nlabels = parse_labels(labels, &ln, &lv, name);
	if (nlabels < 0 || build_key(key, sizeof(key), name, ln, lv, (uint32_t) nlabels)) {
		RETURN_FALSE;
	}
	if (ensure_local()) {
		RETURN_FALSE;
	}
	e = op_series(name, key, FPMNG_METRIC_HISTOGRAM, NULL, 0);
	if (!e) {
		RETURN_FALSE;
	}
	for (k = 0; k < e->bucket_count; k++) {
		if (value <= e->buckets[k]) {
			e->v[k] += 1;
			break;
		}
	}
	e->v[FPMNG_METRICS_BUCKETS_MAX] += value;
	e->v[FPMNG_METRICS_BUCKETS_MAX + 1] += 1;
	RETURN_TRUE;
}

PHP_FUNCTION(fpm_metric_render)
{
	char *text;
	size_t len;
	zend_string *zs;

	if (fpmng_metrics_render_text(&text, &len)) {
		RETURN_FALSE;
	}
	if (!text) {
		text = malloc(1);
		if (!text) {
			RETURN_FALSE;
		}
		text[0] = '\0';
		len = 0;
	}
	/* the render_text buffer is malloc'ed, not emalloc — copy into a
	 * zend_string and free the original */
	zs = zend_string_init(text, len, 0);
	free(text);
	if (!zs) {
		RETURN_FALSE;
	}
	RETURN_STR(zs);
}

/* ===== module ===== */

ZEND_BEGIN_ARG_INFO_EX(arginfo_fpm_metric_register, 0, 0, 2)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, type, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, help, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, buckets, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_fpm_metric_inc, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, by, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, labels, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_fpm_metric_set, 0, 0, 2)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, labels, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_fpm_metric_observe, 0, 0, 2)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, labels, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_fpm_metric_render, 0, 0, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry fpmng_metrics_functions[] = {
	PHP_FE(fpm_metric_register,	arginfo_fpm_metric_register)
	PHP_FE(fpm_metric_inc,		arginfo_fpm_metric_inc)
	PHP_FE(fpm_metric_set,		arginfo_fpm_metric_set)
	PHP_FE(fpm_metric_observe,	arginfo_fpm_metric_observe)
	PHP_FE(fpm_metric_render,	arginfo_fpm_metric_render)
	PHP_FE_END
};

PHP_MINIT_FUNCTION(fpmng_metrics)
{
	REGISTER_INI_ENTRIES();
	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(fpmng_metrics)
{
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}

PHP_MINFO_FUNCTION(fpmng_metrics)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "fpmng_metrics", "enabled");
	php_info_print_table_row(2, "series limit per worker", "fpmng_metrics.series_limit");
	php_info_print_table_row(2, "backend", m_mode_shm ? "shared memory" : "process local (CLI)");
	php_info_print_table_end();
}

zend_module_entry fpmng_metrics_module_entry = {
	STANDARD_MODULE_HEADER,
	"fpmng_metrics",
	fpmng_metrics_functions,
	PHP_MINIT(fpmng_metrics),
	PHP_MSHUTDOWN(fpmng_metrics),
	NULL,
	NULL,
	PHP_MINFO(fpmng_metrics),
	PHP_FPMNG_METRICS_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_FPMNG_METRICS
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(fpmng_metrics)
#endif
