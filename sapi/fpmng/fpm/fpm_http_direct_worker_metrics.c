/* fpm-ng: see fpm_http_direct_worker_metrics.h. */

#include "fpm_config.h"

#include <stdlib.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_type.h"
#include "fpm_scoreboard.h"
#include "fpm_shm.h"
#include "fpm_http_direct_worker_metrics.h"
#include "zlog.h"

/* One slot per potential child, same shape fpm_http_direct_ops.c uses for the
 * classic executor's live gauges -- see that file's fpm_http_direct_ops_slot
 * for the precedent this copies. Both fields are pure gauges (this child's
 * current fw.pending/fw.watchers size), so there is nothing to accumulate: a
 * child overwrites its own slot on every change and the reader sums slots. */
struct fpm_worker_metrics_slot {
	unsigned long pending;
	unsigned long watchers;
};

struct fpm_worker_metrics_shared {
	unsigned nslots;
	struct fpm_worker_metrics_slot slots[];
};

/* wp -> shared segment, same registry-by-linked-list shape as
 * fpm_http_direct_ops.c and fpm_pool_supervisor.c use for the same reason:
 * fpm_worker_pool_s is upstream's struct, and a handful of pools of this type
 * in one master does not need a second allocator. */
struct fpm_worker_metrics_entry {
	struct fpm_worker_pool_s *wp;
	struct fpm_worker_metrics_shared *shared;
	struct fpm_worker_metrics_entry *next;
};

static struct fpm_worker_metrics_entry *fpm_worker_metrics_registry;

struct fpm_worker_metrics {
	struct fpm_worker_metrics_slot *slot;	/* NULL if the pool has no shared segment */
};

static struct fpm_worker_metrics_shared *fpm_worker_metrics_shared_get(struct fpm_worker_pool_s *wp)
{
	struct fpm_worker_metrics_entry *e;

	for (e = fpm_worker_metrics_registry; e; e = e->next) {
		if (e->wp == wp) {
			return e->shared;
		}
	}
	return NULL;
}

int fpm_http_direct_worker_metrics_init_main(struct fpm_worker_pool_s *wp)
{
	struct fpm_worker_metrics_entry *entry;
	struct fpm_worker_metrics_shared *shared;
	unsigned nslots = (unsigned) (wp->config->pm_max_children > 0 ? wp->config->pm_max_children : 1);

	if (fpm_worker_metrics_shared_get(wp)) {
		/* Same re-entrancy note as fpm_http_direct_ops_init_main(): fpm_conf.c
		 * can re-run a pool's init_main inside one master process, and a
		 * second segment here would be one no child ever gets a pointer to. */
		return 0;
	}
	shared = fpm_shm_alloc(sizeof(*shared) + nslots * sizeof(shared->slots[0]));
	if (!shared) {
		zlog(ZLOG_ERROR, "[pool %s] http-direct worker: cannot allocate the pending/watcher gauges",
			wp->config->name);
		return -1;
	}
	shared->nslots = nslots;
	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		zlog(ZLOG_ERROR, "[pool %s] http-direct worker: cannot register the pending/watcher gauges",
			wp->config->name);
		return -1;
	}
	entry->wp = wp;
	entry->shared = shared;
	entry->next = fpm_worker_metrics_registry;
	fpm_worker_metrics_registry = entry;

	return 0;
}

struct fpm_worker_metrics *fpm_http_direct_worker_metrics_init_child(struct fpm_worker_pool_s *wp)
{
	struct fpm_worker_metrics *m = calloc(1, sizeof(*m));
	struct fpm_worker_metrics_shared *shared;
	struct fpm_scoreboard_s *scoreboard;
	struct fpm_scoreboard_proc_s *proc;

	if (!m) {
		return NULL;
	}
	shared = fpm_worker_metrics_shared_get(wp);
	/* The child's own slot is its scoreboard index, same reasoning as
	 * fpm_http_direct_ops_init_child(): the scoreboard already hands each
	 * child a private, stable-for-its-life index, so reusing it avoids a
	 * second allocator with the same lifetime and the same failure modes. */
	scoreboard = fpm_scoreboard_get();
	proc = scoreboard ? fpm_scoreboard_proc_get(scoreboard, -1) : NULL;
	if (shared && proc) {
		size_t index = (size_t) (proc - scoreboard->procs);

		if (index < shared->nslots) {
			m->slot = &shared->slots[index];
			/* Zeroed here, not left to whatever the previous occupant of
			 * this index published: a child recycled by pm.max_requests or
			 * killed mid-request leaks exactly these two gauges, the same
			 * leak fpm_http_direct_ops_init_child() clears for
			 * requests_active/conn_live/responses_pending and for the same
			 * reason -- a fresh child has nothing pending and no watchers
			 * yet. */
			m->slot->pending = 0;
			m->slot->watchers = 0;
		}
	}
	return m;
}

void fpm_http_direct_worker_metrics_free(struct fpm_worker_metrics *m)
{
	free(m);
}

void fpm_worker_metrics_publish(struct fpm_worker_metrics *m, unsigned long pending, unsigned long watchers)
{
	if (!m || !m->slot) {
		return;
	}
	m->slot->pending = pending;
	m->slot->watchers = watchers;
}

int fpm_http_direct_worker_live_gauges(struct fpm_worker_pool_s *wp, struct fpm_pool_live_gauge_s out[FPM_POOL_LIVE_GAUGES_MAX])
{
	struct fpm_worker_metrics_shared *shared = fpm_worker_metrics_shared_get(wp);
	unsigned long pending = 0, watchers = 0;
	unsigned i;

	if (!shared) {
		/* Never reached in practice -- init_main runs for every pool of this
		 * type before the first fork -- but a missing segment reports zero
		 * gauges rather than guessing, the same "skip rather than a page of
		 * zeroes that looks like a measurement" rule fpm_operator_pages.c's
		 * own collect() follows for a type with neither serves_requests nor
		 * .status. */
		return 0;
	}
	/* Sums every child's slot: a worker pool may run pm.max_children > 1
	 * workers, and each holds its own fw.pending/fw.watchers independently --
	 * there is no single "the" worker to read from the way a synchronous
	 * per-request executor's idle/active pair already aggregates in the
	 * scoreboard. A slot whose child has already exited but not yet been
	 * replaced still holds its last published value (the same staleness
	 * window fpm_http_direct_ops.c's conn_live/responses_pending/
	 * requests_active gauges already accept) until a new child claims and
	 * zeroes that index. */
	for (i = 0; i < shared->nslots; i++) {
		pending += shared->slots[i].pending;
		watchers += shared->slots[i].watchers;
	}
	out[0].json_key = "worker_pending";
	out[0].help = "Requests accepted by this worker pool but not yet answered (mid-handler or mid-stream).";
	out[0].value = (long) pending;
	out[1].json_key = "worker_watchers";
	out[1].help = "Libevent watchers (fpmng_worker_event_create) currently registered across this worker pool.";
	out[1].value = (long) watchers;
	return 2;
}
