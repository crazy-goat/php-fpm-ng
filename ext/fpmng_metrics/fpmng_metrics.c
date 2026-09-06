/* fpmng_metrics: metryki aplikacyjne z PHP (docs/NOTES.md 3k).
 *
 * Dwa backendy, jeden kod PHP:
 *  - pod fpm-ng: tablice serii w pamieci dzielonej, po jednej na workera
 *    (patrz php_fpmng_metrics.h), wystawiane przez pool.type = status
 *    na /metrics — aplikacja niczego nie serwuje sama;
 *  - pod CLI (i kazdym innym SAPI): tablica w procesie, tekst do
 *    wystawienia zwraca fpm_metric_render().
 *
 * Zalozenia odwzorowane z NOTATKI 3k:
 *  - wszystkie funkcje zwracaja bool; false to wykrycie problemu
 *    (wyczerpanie limitu serii, zla nazwa, konflikt typow), nie ozdoba;
 *  - ostrzeżenie w logu RAZ na temat (tu: na metryke), nie na request;
 *  - register jest opcjonalny — typ wynika z uzytej funkcji
 *    (inc -> counter, set -> gauge, observe -> histogram);
 *  - kardynalnosc: staly limit serii na workera (INI
 *    fpmng_metrics.series_limit), po wyczerpaniu odrzucamy i zwracamy
 *    false, nigdy nie rośniemy;
 *  - histogramy sa, bez exemplary i bez estymacji kwantyli; kubelki sa
 *    czescia etykiet serii (le="..."), wiec dwa workery z roznym zestawem
 *    kubelkow po prostu wyemituja rozne zbiory le i sie nie gryza;
 *  - pod fpm-ng kazda seria dostaje automatyczna etykieta pool="..." —
 *    inaczej jobs_total aplikacji i consumera zlewalyby sie w jedna
 *    serie bez sledu. Etykieta "pool" od uzytkownika jest odrzucana.
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

/* ===== stan procesu ===== */

/* Kubelki domyslne (NOTES 3k: 5 ms — 60 s). */
const double fpmng_metrics_default_buckets[FPMNG_METRICS_DEFAULT_BUCKETS_N] = {
	0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10, 30, 60
};

/* Ustawiane raz przez mastera (dziedziczone po forku). */
static struct fpmng_metrics_shm_s *m_shm = NULL;
static uint32_t m_slot_count = 0;
static uint32_t m_limit = 0;
static zend_bool m_mode_shm = 0;		/* 0 = backend lokalny (CLI) */

/* Wlasny slot procesu: shm — wskaznik do tablicy workera; lokalnie —
 * tablica na stercie. NULL w shm-mode = "nie przypisano mnie" (master,
 * bramka) i wtedy wszystkie operacje zwracaja false. */
static struct fpmng_metrics_slot_s *m_slot = NULL;

/* Wartosc etykiety pool (shm mode), kopia lokalna — scoreboard moze
 * byc potem zwolniony przez higiene dziecka. */
static char m_pool[33];

/* Ostrzezenia "raz na temat": lista tematow, ktore juz dostaly E_WARNING.
 * Temat = np. pelna nazwa metryki. Prosta tablica — limit ostrzezen
 * zelazny, po jego przekroczeniu milczymy (zbedny szum jest gorszy). */
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

/* ===== geometria tablic ===== */

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
	return (sizeof(struct fpmng_metrics_shm_s) + 7u & ~(size_t) 7u)
		+ (size_t) slots * slot_size(limit);
}

static struct fpmng_metrics_slot_s *shm_slot(struct fpmng_metrics_shm_s *shm, uint32_t i)
{
	size_t hdr = (sizeof(struct fpmng_metrics_shm_s) + 7u) & ~(size_t) 7u;

	return (struct fpmng_metrics_slot_s *) ((char *) shm + hdr + (size_t) i * slot_size(shm->limit));
}

/* ===== API po stronie C (patrz php_fpmng_metrics.h) ===== */

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
	m_slot = NULL;			/* master nie pisze metryk */
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

/* ===== pomocnicze ===== */

/* Ostrzezenie raz na temat. Zwraca 1, jesli ostrzezenie bylo freszkie
 * (pierwszy raz), 0 jesli temat juz byl. */
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

/* Buduje klucz serii: name{k1="v1",k2="v2"} (etykiety posortowane po
 * nazwie — stabilny klucz niezaleznie od kolejnosci w tablicy; w trybie
 * shm etykieta pool idzie zawsze pierwsza). Zwraca 0/-1. */
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

	/* sortowanie przez wstawianie — nlabels <= 8 */
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

/* Zapewnia tablice w trybie lokalnym (CLI). Zwraca 0/-1. */
static int ensure_local(void)
{
	if (m_mode_shm) {
		return m_slot ? 0 : -1;	/* shm-mode bez attachu: master/bramka */
	}
	if (m_slot) {
		return 0;
	}
	m_limit = fpmng_metrics_series_limit();
	m_slot_count = 1;
	m_slot = calloc(1, slot_size(m_limit));
	return m_slot ? 0 : -1;
}

/* Znajduje serie we WLASNYM slocie po kluczu. */
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

/* Definicja metryki (register) we wlasnym slocie: wpis o kluczu rownym
 * samej nazwie. NULL = brak. */
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

/* Nowa seria we wlasnym slocie; NULL przy wyczerpanym limicie
 * (z ostrzezeniem raz na metryke, z nazwa — bez tego operator nie
 * znajdzie winowajcy, NOTES 3k). */
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

/* ===== operacje ===== */

/* Przygotowuje serie do operacji typu `type`. Zwraca NULL przy bledzie
 * (juz ostrzezone). nbuckets/buckets tylko dla histogramu. */
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

	/* Kubelki: z definicji (register) tej nazwy, jesli jest; inaczej
	 * domyslne. Definicja jest we wlasnym slocie — register wywolany w
	 * tym samym procesie wystarcza, bo zwykle leci na starcie kazdego
	 * requestu/zadania. */
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

/* ===== render (C API, bez ZEND_API — wolane tez z pool.type = status) ===== */

struct agg_s {
	char *key;
	char *name;			/* klucz bez etykiet, do HELP/TYPE */
	char help[FPMNG_METRICS_HELP_MAX];
	uint8_t type;
	double buckets[FPMNG_METRICS_BUCKETS_MAX];
	uint16_t nbuckets;
	double v[FPMNG_METRICS_BUCKETS_MAX + 2];	/* jak w entry: counts..., sum, count */
	double value;			/* counter/gauge: suma albo max */
	int have_type_conflict;
};

/* Metadane metryki (register): wpis o golym kluczu, bez etykiet. W trybie
 * shm to NIE jest seria (kazda seria dostaje pool="...", wiec goly klucz
 * nigdy nie zderzy sie z kluczem z inc/set/observe) — trzymamy z niego
 * tylko HELP/TYPE/kubelki. W trybie lokalnym (CLI, bez etykiety pool) goly
 * klucz JEST seria i wartosc tez. */
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

/* nazwa metryki = klucz do pierwszego '{' albo caly klucz */
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

/* Emituje serie <base><suffix>{<inner>,le="<le>"} — Prometheus wymaga
 * przyrostka _bucket/_sum/_count PRZED klamra etykiet, a klucz serii w
 * magazynie wyglada jak name{pool="..",k="v"}. le = gotowy literal etykiety
 * ("0.5", "+Inf") albo NULL dla _sum/_count. */
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
		size_t inner_len = strlen(brace + 1);	/* bez '}' */

		if (inner_len > 1) {	/* sa jakies etykiety wewnatrz */
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

/* Literal granicy kubelka: najkrotsza reprezentacja bez utraty sensu */
static void bucket_literal(char *out, size_t outsz, double le)
{
	snprintf(out, outsz, "%.15g", le);
}

int fpmng_metrics_render_text(char **out, size_t *out_len)
{
	struct rbuf_s b = {0};
	struct agg_s **aggs = NULL;
	size_t naggs = 0, cap = 0;
	uint32_t si, ei;
	struct fpmng_metrics_shm_s *shm = m_shm;
	uint32_t slots = m_slot_count;
	uint32_t limit = m_limit;
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
		slots = shm->slots;
		limit = shm->limit;
	} else {
		/* CLI: render tego, co jest w procesie; pusty wynik jest OK */
		if (!m_slot) {
			*out = malloc(1);
			if (*out) {
				(*out)[0] = '\0';
			}
			*out_len = 0;
			return 0;
		}
		slots = 1;
		limit = m_limit;
		is_local = 1;
	}

	/* Najpierw definicje (register): w trybie shm goly klucz to metadane,
	 * nie seria. W trybie lokalnym goly klucz jest serii (inc bez etykiet
	 * daje dokladnie ten klucz), wiec tam nie wydzielamy. */
	struct meta_s **metas = NULL;
	size_t nmetas = 0, mcap = 0;

	if (m_mode_shm) {
		for (si = 0; si < slots; si++) {
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
						struct meta_s **nm = realloc(metas, mcap * sizeof(*nm));
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

	/* agregacja: po kluczu pelnym (z etykietami) */
	for (si = 0; si < slots; si++) {
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
				/* definicja (register) — metadane zebrane wyzej */
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
					struct agg_s **na = realloc(aggs, cap * sizeof(*na));
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
					goto fail;
				}
				a->type = e->type;
				aggs[naggs++] = a;
			}

			/* metadane z definicji/first-wins */
			if (e->help[0] && !a->help[0]) {
				strlcpy(a->help, e->help, sizeof(a->help));
			}
			if (e->type != a->type) {
				/* konflikt typow miedzy slotami (rozny kod w roznych
				 * workerach) — nie laczymy wartosci, emitujemy je
				 * osobno ponizej po typie pierwszego; patrz nizej. */
				a->have_type_conflict = 1;
			}
			if (e->type == FPMNG_METRIC_HISTOGRAM
					&& e->bucket_count > a->nbuckets
					&& a->type == FPMNG_METRIC_HISTOGRAM) {
				/* zapamietujemy najwiekszy znany zestaw kubelkow,
				 * zebysmy wiedzieli dla jakich le emitoir kontrolne 0 */
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
						/* porownanie pierwszy-raz: uzywamy count jako
						 * znacznika "widziano juz wartosc" */
						a->value = e->v[0];
					}
					a->v[FPMNG_METRICS_BUCKETS_MAX + 1] = 1;
					break;
				case FPMNG_METRIC_HISTOGRAM: {
					uint16_t k;
					/* zliczenia per kubelek: histogram ma le jako
					 * czesc klucza wyjsciowego, wiec zsumuj po
					 * identycznych granicach; counts w a->v[0..]
					 * rosnie wg bucket_count serii WEJSCIOWEJ */
					for (k = 0; k < e->bucket_count; k++) {
						/* zlokalizuj le w a->buckets */
						uint16_t m;
						for (m = 0; m < a->nbuckets; m++) {
							if (a->buckets[m] == e->buckets[k]) {
								break;
							}
						}
						if (m == a->nbuckets) {
							/* granica nieznana a->buckets — dojeta przy
							 * emitowaniu przez posortowane zebranie;
							 * na razie pomin (rzadkie, tylko konflikt
							 * kubelkow miedzy workerami) */
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

	/* stabilne wyjscie: serie posortowane po kluczu — serie tej samej
	 * metryki (ten sam prefix przed '{') laduja obok siebie */
	{
		struct agg_s **sorted = malloc(naggs * sizeof(*sorted));

		if (!sorted) {
			goto fail;
		}
		memcpy(sorted, aggs, naggs * sizeof(*sorted));
		qsort(sorted, naggs, sizeof(*sorted), agg_cmp);

		/* HELP/TYPE raz na metryke: przy pierwszej serii tej nazwy */
		names = malloc(naggs * sizeof(*names));
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
				/* posortowane granice kubelkow */
				double le[FPMNG_METRICS_BUCKETS_MAX];
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

/* ===== funkcje PHP ===== */

/* argumenty etykiet: HashTable string => string. Rozbija do rownoleglych
 * tablic nazw/wartosci. Zwraca liczbe etykiet albo -1. */
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
			/* zarezerwowane — dodaje je fpm-ng, patrz komentarz w naglowku pliku */
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
		/* nieposortowane kubelki sa poprawnym zestawem, ale kolejnosc
		 * le na wyjsciu i tak sortujemy przy renderze */
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
		/* zmiana kubelkow po obserwacjach zmienilaby sens zliczen;
		 * pozwalalismy tylko na pustej serii definicji */
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
	/* set dziala zarowno dla gauge (suma) jak i gauge_max (maksimum) */
	e = find_series(key);
	t = e ? e->type : FPMNG_METRIC_GAUGE_SUM;
	if (t != FPMNG_METRIC_GAUGE_SUM && t != FPMNG_METRIC_GAUGE_MAX) {
		char topic[80];
		snprintf(topic, sizeof(topic), "type:%s", key);
		warn_once(topic, "metric type conflict, operation rejected for series", key);
		RETURN_FALSE;
	}
	if (!e) {
		/* definicja moze deklarowac gauge_max */
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
	/* bufor z render_text jest malloc-owy, nie emalloc — kopia do
	 * zend_string i zwolnienie oryginalu */
	zs = zend_string_init(text, len, 0);
	free(text);
	if (!zs) {
		RETURN_FALSE;
	}
	RETURN_STR(zs);
}

/* ===== modul ===== */

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
