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

/* Master, after fpm_conf_init_main(), before child fork. Failure does NOT kill
 * FPM (application metrics are an add-on, not the foundation) — returns -1 and
 * logs; all fpm_metric_* functions then return false. */
int fpm_metrics_init_main(void);

/* Worker child, at run_child: BEFORE child_main and BEFORE
 * fpm_cleanups_run(CHILD) — after that, the pool list and config disappear, but
 * we need the number of earlier pools' pm.max_children and our own name. */
void fpm_metrics_child_init(void);

#endif
