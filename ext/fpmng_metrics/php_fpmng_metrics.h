/* fpmng_metrics: application-metrics store fed from PHP (NOTES 3k).
 *
 * The header intentionally has no dependency on php.h — the SAPI side
 * (sapi/fpmng/fpm/fpm_metrics.c) also includes it, because it needs the shm
 * layout and a few functions; pulling the whole ZEND_API in there would make
 * no sense.
 *
 * Memory model (NOTES 3k, "PER-WORKER SLOTS, not atomics"): every worker
 * writes to its OWN series array in shared memory, with no synchronization
 * at all; aggregation happens at read time (render). The slot is keyed by a
 * GLOBAL worker index (sum of pm.max_children of earlier pools in the config
 * plus the index from the worker's own scoreboard), not by pid — so
 * recycling after pm.max_requests does not zero the counters.
 *
 * The same code runs under CLI: no shared memory, a process-local array
 * (one slot), and the script exposes the text through fpm_metric_render().
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
/* name{ k="v", ... } — must fit name + the pool label + LBL_MAX labels */
#define FPMNG_METRICS_KEY_MAX      384

#define PHP_FPMNG_METRICS_VERSION "0.1.0"

/* A plain macro in an ext header — for the static build (internal_functions*).
 * The #define itself is harmless on the SAPI side too, which includes this
 * header (it is not used there). The extern declaration only when
 * zend_modules.h has already been included (php.h) — on the SAPI side it
 * has, because fpm.h starts with php.h; plain C (e.g. a future other
 * consumer) gets nothing. */
#ifdef MODULES_H
extern zend_module_entry fpmng_metrics_module_entry;
#endif
#define phpext_fpmng_metrics_ptr &fpmng_metrics_module_entry

/* Default histogram buckets: 5 ms to 60 s (NOTES 3k — consumer jobs take
 * longer than requests, so further out than typical HTTP latencies). */
extern const double fpmng_metrics_default_buckets[13];
#define FPMNG_METRICS_DEFAULT_BUCKETS_N 13

enum fpmng_metric_type_e {
	FPMNG_METRIC_COUNTER = 1,
	FPMNG_METRIC_GAUGE_SUM,
	FPMNG_METRIC_GAUGE_MAX,
	FPMNG_METRIC_HISTOGRAM,
};

/* One series entry in ONE worker's (own!) array. Field order: doubles first
 * (8-byte alignment, no padding), then chars, small fields last.
 *
 * v[] has the fixed size BUCKETS_MAX+2, independent of this series' bucket
 * count, so the positions of the sum and the count do not depend on
 * bucket_count:
 *   histogram:  v[0..bucket_count-1] = bucket counts
 *               v[BUCKETS_MAX]   = sum of observed values
 *               v[BUCKETS_MAX+1] = number of observations
 *   counter:    v[0] = counter
 *   gauge_*:    v[0] = value
 */
struct fpmng_metrics_entry_s {
	double v[FPMNG_METRICS_BUCKETS_MAX + 2];
	double buckets[FPMNG_METRICS_BUCKETS_MAX];
	char key[FPMNG_METRICS_KEY_MAX];
	char help[FPMNG_METRICS_HELP_MAX];
	uint16_t bucket_count;		/* histogram: number of buckets */
	uint8_t type;			/* enum fpmng_metric_type_e */
	uint8_t in_use;
};

/* The series array of ONE worker. Only the owner writes; read at render
 * time by other processes (pool.type = status, render under CLI). */
struct fpmng_metrics_slot_s {
	uint32_t used;			/* grows, never shrinks */
	/* struct fpmng_metrics_entry_s entries[limit]; */
};

/* Header of the shared-memory region, filled once by the master
 * (fpmng_metrics_shm_init) before children fork. */
struct fpmng_metrics_shm_s {
	uint32_t magic;
	uint32_t slots;			/* number of worker slots */
	uint32_t limit;			/* series per slot */
	/* struct fpmng_metrics_slot_s slot_tables[slots]; */
};

#define FPMNG_METRICS_MAGIC 0x4e474d54u	/* "NGMT" */

/* ===== C-side API, called from sapi/fpmng/fpm/fpm_metrics.c ===== */

/* Size of the region for a given number of slots and series limit. */
size_t fpmng_metrics_shm_size(uint32_t slots, uint32_t limit);

/* Master, after parsing the configuration, before forking: prepares the
 * region (mem allocated by the SAPI side through fpm_shm_alloc). */
void fpmng_metrics_shm_init(void *mem, size_t size, uint32_t slots, uint32_t limit);

/* Worker child: from now on the PHP functions write to the slot with this
 * index; pool_name becomes the automatic pool="..." label of every series.
 * Without this call the functions in a gateway/master process return false. */
void fpmng_metrics_child_attach(uint32_t slot_index, const char *pool_name);

/* Series limit from INI (fpmng_metrics.series_limit). Called by the master. */
uint32_t fpmng_metrics_series_limit(void);

/* The full Prometheus text of ALL series (aggregation across slots: sums for
 * counters/gauge_sum, the maximum for gauge_max, bucket sums for
 * histograms). Malloc'ed buffer, freed by the caller. Returns 0/-1. Touches
 * neither ZEND_API nor zend memory — also called from a pool.type = status
 * child, without any request context. */
int fpmng_metrics_render_text(char **out, size_t *len);

/* The same, restricted to the half-open slot range [first_slot, first_slot +
 * slot_count). A pool owns a contiguous run of slots (see the memory-model
 * note above: the index is the sum of earlier pools' pm.max_children plus the
 * worker's own scoreboard index), so a range is how a per-pool endpoint asks
 * for its own pool's series and nobody else's (issue #276). The range is
 * clamped, not validated: asking for slots that do not exist renders an empty
 * page. Under CLI there is one process-local slot and the range is ignored. */
int fpmng_metrics_render_range(uint32_t first_slot, uint32_t slot_count, char **out, size_t *len);

#endif
