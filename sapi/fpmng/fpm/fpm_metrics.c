/* fpm-ng: glue metryk aplikacyjnych (NOTES 3k). Patrz fpm_metrics.h. */

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
		/* zero poolow z procesami (sam pool status ma wymuszone 1) —
		 * nie ma komu pisac metryk, region niepotrzebny */
		return 0;
	}

	limit = fpmng_metrics_series_limit();
	size = fpmng_metrics_shm_size(slots, limit);
	mem = fpm_shm_alloc(size);
	if (!mem) {
		zlog(ZLOG_ERROR, "metrics: nie udalo sie zaalokowac %zu B pamieci dzielonej "
			"(%u slotow x %u serii) — metryki aplikacyjne wylaczone", size, slots, limit);
		return -1;
	}

	fpmng_metrics_shm_init(mem, size, slots, limit);
	zlog(ZLOG_DEBUG, "metrics: shm %zu B, %u slotow workera, limit %u serii/slot",
		size, slots, limit);
	return 0;
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

	/* Pool biezacego dziecka. Respawnione w petli zdarzen dzieci docieraja
	 * do run_child bez wskaznika na swoj pool — dokladnie dlatego
	 * fpm_pool_type_current_pool() dopasowuje przez scoreboard. */
	wp = fpm_pool_type_current_pool();
	if (!wp) {
		return;
	}

	sb = fpm_scoreboard_get();
	if (!sb) {
		return;
	}

	/* Wlasny indeks ze scoreboardu: proc_get(NULL, -1) zwraca proces
	 * biezacego dziecka (dziecko ustawia fpm_scoreboard_i w
	 * fpm_child_resources_use). Indeks = offset w tablicy procs. */
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

	/* Nazwa poola ze scoreboardu (nie z configu — scoreboard przezywa
	 * cleanups dziecka, a i tak jestesmy przed nimi; przy okazji nie
	 * dotykamy po tym punkcie niczego, co cleanups zwalnia). */
	fpmng_metrics_child_attach(slot, sb->pool);
}
/* }}} */
