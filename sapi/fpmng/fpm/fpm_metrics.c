/* fpm-ng: glue for application metrics (NOTES 3k). See fpm_metrics.h. */

#include "fpm_config.h"

#include <string.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_metrics.h"
#include "fpm_scoreboard.h"
#include "fpm_shm.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_type.h"
#include "php_fpmng_metrics.h"
#include "zlog.h"

int fpm_metrics_init_main(void) /* {{{ */
{
	struct fpm_worker_pool_s *wp;
	uint32_t slots = 0;
	uint32_t limit;
	size_t size;
	void *mem;

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		if (wp->config->pm_max_children > 0) {
			slots += (uint32_t) wp->config->pm_max_children;
		}
	}

	if (!slots) {
		/* no pool declares a worker — nobody would write metrics, so the
		 * region is not needed */
		return 0;
	}

	limit = fpmng_metrics_series_limit();
	size = fpmng_metrics_shm_size(slots, limit);
	mem = fpm_shm_alloc(size);
	if (!mem) {
		zlog(ZLOG_ERROR, "metrics: failed to allocate %zu B of shared memory "
			"(%u slots x %u series) — application metrics disabled", size, slots, limit);
		return -1;
	}

	fpmng_metrics_shm_init(mem, size, slots, limit);
	zlog(ZLOG_DEBUG, "metrics: shm %zu B, %u slotow workera, limit %u serii/slot",
		size, slots, limit);
	return 0;
}
/* }}} */

int fpm_metrics_render_pool(struct fpm_worker_pool_s *wp, char **out, size_t *len) /* {{{ */
{
	struct fpm_worker_pool_s *p;
	uint32_t base = 0;

	/* The same walk fpm_metrics_child_init() does, from the other side: there
	 * the child computes its own slot, here the operator endpoint computes the
	 * run of slots its pool owns. Both derive it from configuration order and
	 * pm.max_children, so they cannot disagree unless the walk itself changes
	 * -- which is why it is spelled the same way twice rather than cached in
	 * the pool, where a reload could leave it stale. */
	for (p = fpm_worker_all_pools; p && p != wp; p = p->next) {
		if (p->config->pm_max_children > 0) {
			base += (uint32_t) p->config->pm_max_children;
		}
	}

	if (wp->config->pm_max_children <= 0) {
		/* No workers, so no slots and nothing that could ever have been
		 * written. Render the empty page rather than the whole region. */
		*out = NULL;
		*len = 0;
		return 0;
	}

	return fpmng_metrics_render_range(base, (uint32_t) wp->config->pm_max_children, out, len);
}
/* }}} */

void fpm_metrics_child_init(void) /* {{{ */
{
	struct fpm_worker_pool_s *wp;
	struct fpm_worker_pool_s *p;
	struct fpm_scoreboard_s *sb;
	struct fpm_scoreboard_proc_s *proc;
	uint32_t base = 0;
	uint32_t slot;

	/* The current child's pool. Children respawned by the event loop reach
	 * run_child without a pointer to their pool — exactly why
	 * fpm_pool_type_current_pool() matches through the scoreboard. */
	wp = fpm_pool_type_current_pool();
	if (!wp) {
		return;
	}

	sb = fpm_scoreboard_get();
	if (!sb) {
		return;
	}

	/* Own index from the scoreboard: proc_get(NULL, -1) returns the current
	 * child's process (the child sets fpm_scoreboard_i in
	 * fpm_child_resources_use). Index = offset in the procs array. */
	proc = fpm_scoreboard_proc_get(sb, -1);
	if (!proc) {
		return;
	}

	for (p = fpm_worker_all_pools; p && p != wp; p = p->next) {
		if (p->config->pm_max_children > 0) {
			base += (uint32_t) p->config->pm_max_children;
		}
	}

	slot = base + (uint32_t) (proc - sb->procs);

	/* Pool name from the scoreboard (not from config — the scoreboard
	 * survives the child's cleanups, and we run before them anyway; as a
	 * bonus we touch nothing past this point that cleanups free). */
	fpmng_metrics_child_attach(slot, sb->pool);
}
/* }}} */
