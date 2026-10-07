/* fpm-ng: glue for application metrics (NOTES 3k). See fpm_metrics.h. */

#include "fpm_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_metrics.h"
#include "fpm_reload_shm.h"
#include "fpm_scoreboard.h"
#include "fpm_shm.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_type.h"
#include "php_fpmng_metrics.h"
#include "zlog.h"

/* Slot range of each pool in this generation, by pool name. Derived once by
 * fpm_metrics_init_main(): the sum-of-earlier-pools walk it replaced cannot
 * keep a spared pool's workers on the slots they were bound to when an earlier
 * pool's pm.max_children changed across a selective reload (issue #537). On an
 * ordinary start the two give the same numbers. */
struct fpm_metrics_range_s {
	struct fpm_metrics_range_s *next;
	const char *name; /* wp->config->name: lives as long as the generation; NULL = reserved */
	uint32_t base, count;
};

static struct fpm_metrics_range_s *ranges = NULL;

static int fpm_metrics_range_of(const struct fpm_worker_pool_s *wp, uint32_t *base) /* {{{ */
{
	struct fpm_metrics_range_s *r;

	for (r = ranges; r; r = r->next) {
		if (r->name && strcmp(r->name, wp->config->name) == 0) {
			*base = r->base;
			return 1;
		}
	}
	return 0;
}
/* }}} */

int fpm_metrics_pool_range(const struct fpm_worker_pool_s *wp, uint32_t *base, uint32_t *count) /* {{{ */
{
	struct fpm_metrics_range_s *r;

	for (r = ranges; r; r = r->next) {
		if (r->name && strcmp(r->name, wp->config->name) == 0) {
			*base = r->base;
			*count = r->count;
			return 1;
		}
	}
	return 0;
}
/* }}} */

static int fpm_metrics_overlaps(uint32_t base, uint32_t count) /* {{{ */
{
	struct fpm_metrics_range_s *r;

	for (r = ranges; r; r = r->next) {
		if (base < r->base + r->count && r->base < base + count) {
			return 1;
		}
	}
	return 0;
}
/* }}} */

/* Lowest base at which `count` slots fit between the ranges already placed.
 * With nothing placed that is 0, then the end of the previous pool: the old
 * sequential layout. */
static uint32_t fpm_metrics_first_fit(uint32_t count) /* {{{ */
{
	struct fpm_metrics_range_s *r;
	uint32_t base = 0;
	int moved = 1;

	while (moved) {
		moved = 0;
		for (r = ranges; r; r = r->next) {
			if (base < r->base + r->count && r->base < base + count) {
				base = r->base + r->count;
				moved = 1;
			}
		}
	}
	return base;
}
/* }}} */

static void fpm_metrics_add_range(const char *name, uint32_t base, uint32_t count) /* {{{ */
{
	struct fpm_metrics_range_s *r = calloc(1, sizeof(*r));

	if (!r) {
		return;
	}
	r->name = name;
	r->base = base;
	r->count = count;
	r->next = ranges;
	ranges = r;
}
/* }}} */

/* Slots of a replaced pool in the old generation: kept free of new ranges
 * because its #329 survivor can still be writing to its old slot, and two
 * writers on one slot race (issue #537). */
static void fpm_metrics_reserve(uint32_t base, uint32_t count) /* {{{ */
{
	fpm_metrics_add_range(NULL, base, count);
}
/* }}} */

/* Zero `n` slots from `first` in the region. A hole punched in the memfd gives
 * zeros back and frees the pages (the region is sparse by design: memset would
 * fault in every page, GiBs with a large pm.max_children); memset only if the
 * filesystem cannot punch. */
static void fpm_metrics_clear_slots(int fd, void *mem, size_t header, size_t slot_size, uint32_t first, uint32_t n) /* {{{ */
{
	size_t off = header + (size_t) first * slot_size, len = (size_t) n * slot_size;

#if defined(__linux__) && defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
	if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t) off, (off_t) len) == 0) {
		return;
	}
#else
	(void) fd;
#endif
	memset((char *) mem + off, 0, len);
}
/* }}} */

int fpm_metrics_init_main(void) /* {{{ */
{
	struct fpm_worker_pool_s *wp;
	uint32_t total = 0;
	uint32_t limit;
	size_t size, slot_size, header, old_slots_size = 0;
	void *mem = NULL;
	int fd = -1;
	int inh_fd = -1, have_inh = 0;
	size_t inh_size = 0;
	uint32_t inh_slots = 0, inh_limit = 0;
	int any_pool = 0;

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		if (wp->config->pm_max_children > 0) {
			any_pool = 1;
		}
	}

	limit = fpmng_metrics_series_limit();

	/* A selective reload (#330) may have carried the previous generation's
	 * region across exec for its spared pools (issue #537). Usable only if
	 * a slot table has the same size, i.e. fpmng_metrics.series_limit did not
	 * change. */
	if (fpm_reload_shm_inherited_metrics(&inh_fd, &inh_size, &inh_slots, &inh_limit)) {
		if (inh_limit == limit && inh_size >= fpmng_metrics_shm_size(inh_slots, limit)) {
			have_inh = 1;
		} else {
			zlog(ZLOG_WARNING, "metrics: the region carried over by a selective reload does not "
							   "match fpmng_metrics.series_limit; spared pools lose their series");
			fpm_reload_shm_metrics_done(0);
		}
	}

	if (!any_pool) {
		/* no pool declares a worker — nobody would write metrics, so the
		 * region is not needed */
		if (have_inh) {
			fpm_reload_shm_metrics_done(0);
		}
		return 0;
	}

	/* Pools a selective reload spared keep the slots their workers are bound
	 * to; everything else is placed around them. */
	if (have_inh) {
		for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
			uint32_t base, count;

			if (wp->config->pm_max_children > 0 && fpm_reload_shm_inherited_range(wp->config->name, &base, &count) && count == (uint32_t) wp->config->pm_max_children && base + count <= inh_slots && !fpm_metrics_overlaps(base, count)) {
				fpm_metrics_add_range(wp->config->name, base, count);
			}
		}
	}
	if (have_inh) {
		fpm_reload_shm_foreach_unspared_range(fpm_metrics_reserve);
	}
	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		uint32_t base;

		if (wp->config->pm_max_children > 0 && !fpm_metrics_range_of(wp, &base)) {
			uint32_t count = (uint32_t) wp->config->pm_max_children;

			fpm_metrics_add_range(wp->config->name, fpm_metrics_first_fit(count), count);
		}
	}
	{
		struct fpm_metrics_range_s *r;

		for (r = ranges; r; r = r->next) {
			if (r->base + r->count > total) { /* reserved ranges count too: a survivor must not hit a truncated page */
				total = r->base + r->count;
			}
		}
	}

	size = fpmng_metrics_shm_size(total, limit);
	slot_size = fpmng_metrics_shm_size(2, limit) - fpmng_metrics_shm_size(1, limit);
	header = fpmng_metrics_shm_size(1, limit) - slot_size;

	if (have_inh) {
		if (ftruncate(inh_fd, (off_t) size) == 0) {
			mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, inh_fd, 0);
			if (mem == MAP_FAILED) {
				mem = NULL;
			}
		}
		if (mem) {
			struct fpm_metrics_range_s *r;

			fd = inh_fd;
			old_slots_size = inh_slots;
			/* A pool that was not spared starts from zero, as it does after a
			 * cold start: its previous workers are gone, and what they wrote is
			 * stale. Only the part the old region covered can hold anything. */
			for (r = ranges; r; r = r->next) {
				uint32_t b, c;

				if (!r->name) {
					continue;
				}
				if (fpm_reload_shm_inherited_range(r->name, &b, &c) && b == r->base && c == r->count) {
					continue;
				}
				if (r->base < old_slots_size) {
					uint32_t n = r->count;

					if (r->base + n > old_slots_size) {
						n = (uint32_t) old_slots_size - r->base;
					}
					fpm_metrics_clear_slots(fd, mem, header, slot_size, r->base, n);
				}
			}
			fpm_reload_shm_metrics_done(1);
		} else {
			zlog(ZLOG_WARNING, "metrics: cannot map the region carried over by a selective reload "
							   "(%s); spared pools lose their series",
					strerror(errno));
			fpm_reload_shm_metrics_done(0);
			while (ranges) { /* the spared pools' old slots are not usable: lay out afresh */
				struct fpm_metrics_range_s *next = ranges->next;

				free(ranges);
				ranges = next;
			}
			total = 0;
			for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
				if (wp->config->pm_max_children > 0) {
					uint32_t count = (uint32_t) wp->config->pm_max_children;

					fpm_metrics_add_range(wp->config->name, fpm_metrics_first_fit(count), count);
					total += count;
				}
			}
			size = fpmng_metrics_shm_size(total, limit);
		}
	}

	if (!mem && fpm_global_config.reload_selective) {
		mem = fpm_reload_shm_alloc(size, &fd);
	}
	if (!mem) {
		fd = -1;
		mem = fpm_shm_alloc(size);
	}
	if (!mem) {
		zlog(ZLOG_ERROR, "metrics: failed to allocate %zu B of shared memory "
						 "(%u slots x %u series) — application metrics disabled",
				size, total, limit);
		return -1;
	}

	fpmng_metrics_shm_init(mem, size, total, limit);
	if (fd >= 0) {
		fpm_reload_shm_register_metrics(fd, size, total, limit);
	}
	zlog(ZLOG_DEBUG, "metrics: shm %zu B, %u slotow workera, limit %u serii/slot",
			size, total, limit);
	return 0;
}
/* }}} */

int fpm_metrics_render_pool(struct fpm_worker_pool_s *wp, char **out, size_t *len) /* {{{ */
{
	uint32_t base = 0;

	if (wp->config->pm_max_children <= 0) {
		/* No workers, so no slots and nothing that could ever have been
		 * written. Render the empty page rather than the whole region. */
		*out = NULL;
		*len = 0;
		return 0;
	}

	/* The same table fpm_metrics_child_init() reads, so the endpoint and the
	 * workers cannot disagree about which slots a pool owns. */
	if (!fpm_metrics_range_of(wp, &base)) {
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

	if (!fpm_metrics_range_of(wp, &base)) {
		return;
	}

	slot = base + (uint32_t) (proc - sb->procs);

	/* Pool name from the scoreboard (not from config — the scoreboard
	 * survives the child's cleanups, and we run before them anyway; as a
	 * bonus we touch nothing past this point that cleanups free). */
	fpmng_metrics_child_attach(slot, sb->pool);
}
/* }}} */
