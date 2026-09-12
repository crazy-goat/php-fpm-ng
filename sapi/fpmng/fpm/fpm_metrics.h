/* fpm-ng: application-metrics glue (NOTES 3k) on the SAPI side.
 *
 * Master: after configuration parsing (pm.max_children for all pools is known),
 * count total worker slots and allocate ONE shared-memory region through
 * fpm_shm_alloc, which is passed to the fpmng_metrics extension
 * (fpmng_metrics_shm_init). Children inherit the mapping after fork.
 *
 * Child: at run_child: (fpm.c), calculate the global worker-slot index and pool
 * name and pass them to fpmng_metrics_child_attach. From then on, PHP
 * fpm_metric_* functions write to the worker's own series table.
 *
 * Worker slot = sum of pm.max_children for earlier pools in configuration order
 * + the index from this pool's scoreboard. Index, not pid — recycling after
 * pm.max_requests then does not zero the counters (NOTES 3k).
 */

#ifndef FPM_METRICS_H
#define FPM_METRICS_H 1

#include <stddef.h>

struct fpm_worker_pool_s;

/* Master, after fpm_conf_init_main(), before child fork. Failure does NOT kill
 * FPM (application metrics are an add-on, not the foundation) — returns -1 and
 * logs; all fpm_metric_* functions then return false. */
int fpm_metrics_init_main(void);

/* Worker child, at run_child: BEFORE child_main and BEFORE
 * fpm_cleanups_run(CHILD) — after that, the pool list and config disappear, but
 * we need the number of earlier pools' pm.max_children and our own name. */
void fpm_metrics_child_init(void);

/* The Prometheus text of ONE pool's application series (issue #276). The
 * per-pool operator endpoint answers with its own pool's numbers only, so it
 * renders the slot range that pool owns -- the run this file hands out in
 * fpm_metrics_child_init() -- instead of aggregating the whole region, which
 * since issue #278 nothing in the master does: there is no endpoint left that
 * reports on a pool other than its own.
 *
 * Called from the operator endpoint's child, which is not one of the pool's
 * workers: everything it reads is the shared region the master allocated, so
 * there is nothing process-local to get wrong. Malloc'ed buffer, freed by the
 * caller; *out may be NULL with *len 0 when the pool has no slots. Returns
 * 0/-1. */
int fpm_metrics_render_pool(struct fpm_worker_pool_s *wp, char **out, size_t *len);

#endif
