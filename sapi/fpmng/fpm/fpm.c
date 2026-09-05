	/* (c) 2007,2008 Andrei Nigmatulin */

#include "fpm_config.h"

#include <stdlib.h> /* for exit */

#include "fpm.h"
#include "fpm_children.h"
#include "fpm_signals.h"
#include "fpm_env.h"
#include "fpm_events.h"
#include "fpm_cleanup.h"
#include "fpm_php.h"
#include "fpm_sockets.h"
#include "fpm_pool_type.h"
#include "fpm_unix.h"
#include "fpm_process_ctl.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_scoreboard.h"
#include "fpm_stdio.h"
#include "fpm_log.h"
#include "zlog.h"

struct fpm_globals_s fpm_globals = {
	.parent_pid = 0,
	.argc = 0,
	.argv = NULL,
	.config = NULL,
	.prefix = NULL,
	.pid = NULL,
	.running_children = 0,
	.error_log_fd = 0,
	.log_level = 0,
	.listening_socket = 0,
	.max_requests = 0,
	.is_child = 0,
	.test_successful = 0,
	.heartbeat = 0,
	.run_as_root = 0,
	.force_stderr = 0,
	.send_config_pipe = {0, 0},
};

enum fpm_init_return_status fpm_init(int argc, char **argv, char *config, char *prefix, char *pid, int test_conf, int run_as_root, int force_daemon, int force_stderr) /* {{{ */
{
	fpm_globals.argc = argc;
	fpm_globals.argv = argv;
	if (config && *config) {
		fpm_globals.config = strdup(config);
	}
	fpm_globals.prefix = prefix;
	fpm_globals.pid = pid;
	fpm_globals.run_as_root = run_as_root;
	fpm_globals.force_stderr = force_stderr;

	if (0 > fpm_php_init_main()           ||
	    0 > fpm_stdio_init_main()         ||
	    0 > fpm_conf_init_main(test_conf, force_daemon) ||
	    0 > fpm_unix_init_main()          ||
	    0 > fpm_scoreboard_init_main()    ||
	    0 > fpm_pctl_init_main()          ||
	    0 > fpm_env_init_main()           ||
	    0 > fpm_signals_init_main()       ||
	    0 > fpm_children_init_main()      ||
	    0 > fpm_sockets_init_main()       ||
	    0 > fpm_worker_pool_init_main()   ||
	    0 > fpm_event_init_main()) {

		if (fpm_globals.test_successful) {
			return FPM_INIT_EXIT_OK;
		} else {
			zlog(ZLOG_ERROR, "FPM initialization failed");
			return FPM_INIT_ERROR;
		}
	}

	if (0 > fpm_conf_write_pid()) {
		zlog(ZLOG_ERROR, "FPM initialization failed");
		return FPM_INIT_ERROR;
	}

	fpm_stdio_init_final();
	zlog(ZLOG_NOTICE, "fpm is running, pid %d", (int) fpm_globals.parent_pid);

	return FPM_INIT_CONTINUE;
}
/* }}} */

/*	children: return listening socket
	parent: never return */
int fpm_run(int *max_requests) /* {{{ */
{
	struct fpm_worker_pool_s *wp;

	/* Inicjalizacja typow pooli przed forkiem dzieci — bramki HTTP dziedzicza
	 * wtedy to samo koncowe stdio co workery. */
	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		const struct fpm_pool_type_s *type = fpm_pool_type_of(wp);

		if (type->init_main && 0 > type->init_main(wp)) {
			zlog(ZLOG_ERROR, "[pool %s] failed to initialize pool type '%s'",
				wp->config->name, type->name);
			fpm_pctl(FPM_PCTL_STATE_TERMINATING, FPM_PCTL_ACTION_SET);
			fpm_event_loop(1);
		}
	}

	/* create initial children in all pools */
	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		int is_parent;

		is_parent = fpm_children_create_initial(wp);

		if (!is_parent) {
			goto run_child;
		}

		/* handle error */
		if (is_parent == 2) {
			fpm_pctl(FPM_PCTL_STATE_TERMINATING, FPM_PCTL_ACTION_SET);
			fpm_event_loop(1);
		}
	}

	/* run event loop forever */
	fpm_event_loop(0);

run_child: /* only workers reach this point */

	/* Typ poola moze przejac dziecko zamiast petli accept (supervisor, cron).
	 * Pool bierzemy ze scoreboardu, bo dzieci wskrzeszane w petli zdarzen
	 * docieraja tu bez wskaznika na swoj pool.
	 *
	 * WAZNE: to musi sie stac PRZED fpm_cleanups_run(FPM_CLEANUP_CHILD)
	 * ponizej. fpm_worker_pool_init_main() (fpm_worker_pool.c, referencja)
	 * rejestruje fpm_worker_pool_cleanup() na FPM_CLEANUP_ALL, czyli i na
	 * CHILD — ta funkcja zwalnia CALA liste fpm_worker_all_pools, wlacznie
	 * z wp->config (free()), i na koniec ustawia fpm_worker_all_pools = NULL.
	 * Zrobione to specjalnie dla zwyklego workera FastCGI, ktory po tym
	 * punkcie juz nigdy nie zagląda do wp/config (dziala dalej wylacznie na
	 * fpm_globals). My (supervisor/cron) potrzebujemy wp->config przez CALY
	 * czas zycia procesu, wiec odczytujemy go, ZANIM zniknie. */
	{
		struct fpm_worker_pool_s *child_wp = fpm_pool_type_current_pool();
		const struct fpm_pool_type_s *type = child_wp ? fpm_pool_type_of(child_wp) : NULL;

		if (type && type->child_main) {
			/* Ten typ przejmuje caly proces na dobre — nie wraca, wiec
			 * pomijamy fpm_cleanups_run(FPM_CLEANUP_CHILD) w ogole: i tak nie
			 * ma juz kodu po tym punkcie, ktory by z niego skorzystal, a
			 * zwolnienie wp/config pod nami zamienilyby kazdy dostep do
			 * konfiguracji w tym procesie w use-after-free. System i tak
			 * odzyska wszystko przy zakonczeniu procesu (exit() wolane z
			 * child_main). */
			type->child_main(child_wp);
			/* nie wraca */
		}
	}

	fpm_cleanups_run(FPM_CLEANUP_CHILD);

	*max_requests = fpm_globals.max_requests;
	return fpm_globals.listening_socket;
}
/* }}} */
