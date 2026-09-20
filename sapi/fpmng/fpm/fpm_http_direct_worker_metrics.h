/* fpm-ng: pool.executor = worker's honest metrics (issue #333).
 *
 * The worker executor calls fpm_request_accepting(false) once for the whole
 * life of the child (fpm_http_direct_worker.c), so it has no per-request
 * scoreboard stage to report -- exactly why fpm_http_direct_worker_rejects
 * refuses operator.status_path, ping.* and access.*. What it DOES know honestly,
 * in real time and without inventing a stage, is:
 *
 *   - how many requests it has actually answered (fw.answered, counted into
 *     the scoreboard's plain requests counter by fpm_http_direct_worker.c --
 *     see the fpm_scoreboard_update() calls next to fw.answered++);
 *   - how many it is currently holding, unanswered (fw.pending's size);
 *   - how many libevent watchers userland has registered (fw.watchers' size).
 *
 * The latter two are per-CHILD state (fw is a file-local static in
 * fpm_http_direct_worker.c, one instance per process) that the operator
 * endpoint's renderer (fpm_operator_pages.c) needs to read from a different
 * process. This file is that channel: one shared-memory slot per potential
 * child, indexed by the child's own scoreboard index -- the same mechanism
 * fpm_http_direct_ops.c already uses for the classic executor's live gauges
 * (fpm_http_direct_ops_slot.responses_pending et al.), reused here rather
 * than invented a second way, per fpm_pool_type_s's per-pool-type contract.
 *
 * No lock: each child writes only its own slot (single writer), and every
 * field is a word-sized plain integer a reader can only ever see as either
 * its previous value or its latest one, never a tear -- the same argument
 * fpm_http_direct_ops.c's slot fields already rely on.
 */

#ifndef FPM_HTTP_DIRECT_WORKER_METRICS_H
#define FPM_HTTP_DIRECT_WORKER_METRICS_H 1

#include "fpm_pool_type.h"

struct fpm_worker_pool_s;
struct fpm_worker_metrics;

/* Master side, before the first fork: allocates the pool's shared slots.
 * Mirrors fpm_http_direct_ops_init_main() -- 0 or -1. Safe to call again for
 * the same pool inside one master process (fpm_conf.c can re-run per-pool
 * init); a second segment is not allocated. */
int fpm_http_direct_worker_metrics_init_main(struct fpm_worker_pool_s *wp);

/* Child side, once at start-up: claims this child's slot (by scoreboard
 * index, like fpm_http_direct_ops_init_child()) and zeroes it -- a fresh
 * child has nothing pending and no watchers yet, whatever its predecessor in
 * this slot left behind. Returns NULL if the pool has no shared slots (master
 * init failed or was never reached), in which case fpm_worker_metrics_publish()
 * below is a no-op: metrics are best-effort, never a reason to fail a request. */
struct fpm_worker_metrics *fpm_http_direct_worker_metrics_init_child(struct fpm_worker_pool_s *wp);

void fpm_http_direct_worker_metrics_free(struct fpm_worker_metrics *m);

/* Publishes this child's current pending/watcher counts. Called right after
 * every mutation of fw.pending/fw.watchers in fpm_http_direct_worker.c (the
 * two tables have exactly one add site and one remove site each -- see
 * fpm_worker_reap() and fpmng_worker_event_create()/_free()) so the gauge is
 * never more than one event stale. Absolute values, not deltas: unlike
 * fpm_http_direct_ops.c's totals, a gauge has nothing to accumulate, and a
 * plain overwrite is what makes a clean shutdown's fpm_worker_metrics_publish(m, 0, 0)
 * (fpm_http_direct_worker.c's teardown) correct without needing to know the
 * previous value. */
void fpm_worker_metrics_publish(struct fpm_worker_metrics *m, unsigned long pending, unsigned long watchers);

/* fpm_pool_type_s.live_gauges for the worker executor: sums every child's
 * slot (a pool may have pm.max_children > 1 workers) into the two gauges
 * fpmng_pool_worker_pending / fpmng_pool_worker_watchers. Called from the
 * operator endpoint's own child -- reads shared memory only, per the
 * live_gauges contract in fpm_pool_type.h. Declared here (not static in the
 * .c) so fpm_pool_type.c can wire it into the fpm_http_direct_worker literal,
 * the same way fpm_http_direct_worker_child_main is. */
int fpm_http_direct_worker_live_gauges(struct fpm_worker_pool_s *wp,
	struct fpm_pool_live_gauge_s out[FPM_POOL_LIVE_GAUGES_MAX]);

#endif
