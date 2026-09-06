/* fpmng_metrics: magazyn metryk aplikacyjnych z PHP (NOTES 3k).
 *
 * Naglowek intencjonalnie bez zaleznosci od php.h — wlacza go takze strona
 * SAPI (sapi/fpmng/fpm/fpm_metrics.c), ktora potrzebuje layoutu shm i paru
 * funkcji, a nie ma sensu wciagac tam calego ZEND_API.
 *
 * Model pamieci (NOTES 3k, "SLOTY PER WORKER, nie atomiki"):
 * kazdy worker pisze do WLASNEJ tablicy serii w pamieci dzielonej, bez
 * zadnej synchronizacji; sumowanie robi sie przy odczycie (render). Slot
 * kluczowany GLOBALNYM indeksem workera (suma pm.max_children poolow
 * wczesniejszych w configu + indeks ze scoreboardu wlasnego poola), nie
 * pidem — po to, zeby recykling po pm.max_requests nie zerowal licznikow.
 *
 * Ten sam kod dziala w CLI: bez pamieci dzielonej, z tablica w procesie
 * (jeden slot), a skrypt wystawia tekst przez fpm_metric_render().
 */

#ifndef PHP_FPMNG_METRICS_H
#define PHP_FPMNG_METRICS_H

#include <stddef.h>
#include <stdint.h>

#define FPMNG_METRICS_NAME_MAX     64
#define FPMNG_METRICS_HELP_MAX     128
#define FPMNG_METRICS_LBLVAL_MAX   128
#define FPMNG_METRICS_LBLNAME_MAX  32
#define FPMNG_METRICS_LBL_MAX      8
#define FPMNG_METRICS_BUCKETS_MAX  32
/* name{ k="v", ... } — musi pomiescic name + etykiete pool + LBL_MAX etykiet */
#define FPMNG_METRICS_KEY_MAX      384

#define PHP_FPMNG_METRICS_VERSION "0.1.0"

/* Zwykle makro w naglowku ext — dla buildu statycznego (internal_functions*).
 * Samo #define jest nieszkodliwe tez po stronie SAPI, ktora wlacza ten
 * naglowek (tam nie jest uzywane). Deklaracja extern tylko wtedy, gdy
 * zend_modules.h zostal juz wciagniety (php.h) — po stronie SAPI jest, bo
 * fpm.h zaczyna sie od php.h; czysty C (np. przyszly inny konsument) nie
 * dostaje nic. */
#ifdef MODULES_H
extern zend_module_entry fpmng_metrics_module_entry;
#endif
#define phpext_fpmng_metrics_ptr &fpmng_metrics_module_entry

/* Domykne kubelki histogramu: 5 ms do 60 s (NOTES 3k — zadania consumera
 * trwaja dluzej niz requesty, wiec dalej niz typowe dla HTTP). */
extern const double fpmng_metrics_default_buckets[13];
#define FPMNG_METRICS_DEFAULT_BUCKETS_N 13

enum fpmng_metric_type_e {
	FPMNG_METRIC_COUNTER = 1,
	FPMNG_METRIC_GAUGE_SUM,
	FPMNG_METRIC_GAUGE_MAX,
	FPMNG_METRIC_HISTOGRAM,
};

/* Jeden wpis serii w tablicy (wlasnej!) jednego workera. Kolejnosc pol:
 * najpierw doubly (wyrownanie 8 bez paddingu), potem chary, na koncu male.
 *
 * v[] ma staly rozmiar BUCKETS_MAX+2, niezalezny od liczby kubelkow tej
 * serii, zeby pozycje sumy i licznika nie zalezaly od bucket_count:
 *   histogram:  v[0..bucket_count-1] = zliczenia kubelkow
 *               v[BUCKETS_MAX]   = suma obserwowanych wartosci
 *               v[BUCKETS_MAX+1] = liczba obserwacji
 *   counter:    v[0] = licznik
 *   gauge_*:    v[0] = wartosc
 */
struct fpmng_metrics_entry_s {
	double v[FPMNG_METRICS_BUCKETS_MAX + 2];
	double buckets[FPMNG_METRICS_BUCKETS_MAX];
	char key[FPMNG_METRICS_KEY_MAX];
	char help[FPMNG_METRICS_HELP_MAX];
	uint16_t bucket_count;		/* histogram: liczba kubelkow */
	uint8_t type;			/* enum fpmng_metric_type_e */
	uint8_t in_use;
};

/* Tablica serii JEDNEGO workera. Pisze wylacznie wlasciciel; czytana
 * przy renderze przez inne procesy (pool.type = status, render w CLI). */
struct fpmng_metrics_slot_s {
	uint32_t used;			/* rosnie, nigdy nie maleje */
	/* struct fpmng_metrics_entry_s entries[limit]; */
};

/* Naglowek regionu w pamieci dzielonej, wypelniany raz przez mastera
 * (fpmng_metrics_shm_init) przed forkiem dzieci. */
struct fpmng_metrics_shm_s {
	uint32_t magic;
	uint32_t slots;			/* liczba slotow workera */
	uint32_t limit;			/* serii na slot */
	/* struct fpmng_metrics_slot_s slot_tables[slots]; */
};

#define FPMNG_METRICS_MAGIC 0x4e474d54u	/* "NGMT" */

/* ===== API po stronie C, wolane przez sapi/fpmng/fpm/fpm_metrics.c ===== */

/* Rozmiar regionu pod zadana liczbe slotow i limit serii. */
size_t fpmng_metrics_shm_size(uint32_t slots, uint32_t limit);

/* Master, po sparsowaniu konfiguracji, przed forkiem: przygotowuje region
 * (mem zaalokowany przez strone SAPI przez fpm_shm_alloc). */
void fpmng_metrics_shm_init(void *mem, size_t size, uint32_t slots, uint32_t limit);

/* Dziecko workera: od teraz funkcje PHP pisza do slotu o tym indeksie;
 * pool_name trafia do automatycznej etykiety pool="..." kazdej serii.
 * Bez tego wywolania funkcji w procesie bramki/mastera zwracaja false. */
void fpmng_metrics_child_attach(uint32_t slot_index, const char *pool_name);

/* Limit serii z INI (fpmng_metrics.series_limit). Wolane przez mastera. */
uint32_t fpmng_metrics_series_limit(void);

/* Pelny tekst Prometheus WSZYSTKICH serii (agregacja po slotach: sumy dla
 * counterow/gauge_sum, maksimum dla gauge_max, sumy kubelkow dla
 * histogramow). Malloc-owany bufor do zwolnienia przez wolajacego.
 * Zwraca 0/-1. Nie dotyka ZEND_API ani pamieci zendowej — wolane takze
 * z dziecka pool.type = status, bez zadnego kontekstu requestu. */
int fpmng_metrics_render_text(char **out, size_t *len);

#endif
