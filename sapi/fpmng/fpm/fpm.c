	/* (c) 2007,2008 Andrei Nigmatulin */

#include "fpm_config.h"

#include <stdlib.h> /* for exit */
#include <string.h>

#include "fpm.h"
#include "fpm_children.h"
#include "fpm_signals.h"
#include "fpm_env.h"
#include "fpm_events.h"
#include "fpm_cleanup.h"
#include "fpm_php.h"
#include "fpm_sockets.h"
#include "fpm_pool_type.h"
#include "fpm_tier.h"
#include "fpm_unix.h"
#include "fpm_process_ctl.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_scoreboard.h"
#include "fpm_stdio.h"
#include "fpm_log.h"
#include "fpm_request.h"
#include "fpm_metrics.h"
#include "fpm_acme_challenge.h"
#include "fpm_libphp_compat.h"
#include "fastcgi.h"
#include "zend_signal.h"
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

	/* Before anything forks: a module registered here is inherited by every
	 * child, and its INI entries exist before fpm_conf_init_main() parses the
	 * configuration. On the from-source build this is a no-op -- see
	 * fpm_libphp_compat.c (issue #216). */
	if (0 > fpmng_libphp_register_bundled_modules() ||
	    0 > fpm_php_init_main()           ||
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

	/* Application metrics (NOTES 3k): shared-memory region for worker slots.
	 * Deliberately OUTSIDE the chain above — failure does not kill FPM because
	 * application metrics are an add-on; fpm_metric_* then return false. */
	fpm_metrics_init_main();

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

	/* Issue #295: the two BETA features that are not pool types, announced
	 * where the pool tiers are announced and through the same function, so
	 * that the wording and the level cannot drift from each other. What is
	 * reported is a property of the BINARY -- the operator gave the configure
	 * flag and cannot read it back off `-v` -- so neither line is conditional
	 * on the configuration using the feature. #269 put both at beta and said
	 * why: this code terminates connections from unauthenticated strangers and
	 * nobody has audited it. */
#ifdef HAVE_FPMNG_TLS
	fpm_tier_announce(FPM_TIER_BETA, NULL,
		"TLS termination, unaudited and network-facing "
		"(this binary was built with --enable-fpmng-tls)");
#endif
#ifdef HAVE_FPMNG_ACME
	fpm_tier_announce(FPM_TIER_BETA, NULL,
		"ACME certificate issuance, unaudited "
		"(this binary was built with --enable-fpmng-acme)");
#endif

	/* The shared HTTP-01 challenge state is global to the process tree, not
	 * a property of any one pool: the process that publishes a token (a
	 * pool.type = cron ACME process) is never the process that answers the
	 * CA (a gateway child of an http pool). Allocated here, unconditionally
	 * and before the first fork, for the reason fpm_acme_challenge.h gives. */
	if (0 > fpm_acme_challenge_init_main()) {
		fpm_pctl(FPM_PCTL_STATE_TERMINATING, FPM_PCTL_ACTION_SET);
		fpm_event_loop(1);
	}

	/* Initialize pool types before child fork — HTTP gateways then inherit the
	 * same final stdio state as workers. */
	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		const struct fpm_pool_type_s *type = fpm_pool_type_of(wp);

		/* Issue #295: the tier of what this pool is about to run, said once,
		 * here. In the master and before the fork, so it costs one line per
		 * pool per start rather than one per child or -- worse -- one per
		 * request, which an operator would learn to filter and then miss.
		 * Nothing is printed for a supported pool; see fpm_tier.h. */
		fpm_pool_type_announce_tier(wp);

		if (0 > fpm_pool_type_prepare_listening_socket(wp)) {
			zlog(ZLOG_ERROR, "[pool %s] failed to prepare listening socket for pool type '%s'",
				wp->config->name, type->name);
			fpm_pctl(FPM_PCTL_STATE_TERMINATING, FPM_PCTL_ACTION_SET);
			fpm_event_loop(1);
		}

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

	/* A pool type may take over the child instead of the accept loop (supervisor,
	 * cron). Find the pool through the scoreboard because children respawned from
	 * the event loop arrive here without a pointer to their pool.
	 *
	 * IMPORTANT: this must happen BEFORE fpm_cleanups_run(FPM_CLEANUP_CHILD)
	 * below. fpm_worker_pool_init_main() (fpm_worker_pool.c, reference) registers
	 * fpm_worker_pool_cleanup() for FPM_CLEANUP_ALL, including CHILD — that
	 * function frees the ENTIRE fpm_worker_all_pools list, including wp->config
	 * (free()), and finally sets fpm_worker_all_pools = NULL. This was deliberate
	 * for an ordinary FastCGI worker, which never looks at wp/config again after
	 * this point (it uses only fpm_globals). We (supervisor/cron) need wp->config
	 * for the ENTIRE process lifetime, so read it BEFORE it disappears. */
	{
		struct fpm_worker_pool_s *child_wp = fpm_pool_type_current_pool();
		const struct fpm_pool_type_s *type = child_wp ? fpm_pool_type_of(child_wp) : NULL;

		/* An ordinary FastCGI worker no longer has wp->config after
		 * fpm_cleanups_run(), so read here anything from pool configuration that
		 * must apply per request. */
		if (child_wp) {
			fpm_request_set_cpu_tracking(child_wp->config->request_cpu_tracking);
		}
		/* Assign the metrics slot BEFORE cleanup — afterwards the pool list and
		 * earlier pools' pm.max_children disappear (see fpm_metrics.c). */
		fpm_metrics_child_init();
		if (type && type->reuses_request_runtime) {
			fcgi_set_optimized_transport(true);
#ifdef HAVE_FPMNG_PERSISTENT_SIGNALS
			zend_signal_use_persistent_handlers(true);
#else
			/* Without patches/0006 this symbol does not exist (build against a
			 * distribution libphp, build/libphp-build.sh). Nothing is lost here:
			 * the same define makes every type with reuses_request_runtime set
			 * refuse to start, so this branch is unreachable in such a build --
			 * see fpm_pool_type_check_build_support(). */
#endif
		}

		if (type && type->child_main) {
			/* This type takes over the entire process permanently — it does not
			 * return, so skip fpm_cleanups_run(FPM_CLEANUP_CHILD) entirely: no code
			 * after this point could use it anyway, and freeing wp/config underneath
			 * us would turn every later configuration access in this process into a
			 * use-after-free. The system reclaims everything when the process exits
			 * (exit() called by child_main). */
			type->child_main(child_wp);
			/* does not return */
		}
	}

	fpm_cleanups_run(FPM_CLEANUP_CHILD);

	*max_requests = fpm_globals.max_requests;
	return fpm_globals.listening_socket;
}
/* }}} */
