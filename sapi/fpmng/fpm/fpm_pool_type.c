/* fpm-ng: pool-type registry. See fpm_pool_type.h. */

#include "fpm_config.h"

#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_type.h"
#include "fpm_http.h"
#include "fpm_http_direct.h"
#include "fpm_http_direct_tls.h"
#include "fpm_http_direct_worker.h"
#include "fpm_http_direct_worker_metrics.h"
#include "fpm_http_direct_ops.h"
#include "fpm_pool_supervisor.h"
#include "fpm_pool_cron.h"
#include "fpm_operator_endpoint.h"
#include "fpm_scoreboard.h"
#include "zlog.h"

/* http.* tunes the gateway, which starts only under pool.type = gateway (issue
 * #388, formerly pool.type = http) — on every other type these directives have
 * nothing to tune. worker.* applies only to
 * pool.executor = worker (issue #331) -- fpm_http_direct_worker_accepts
 * further down carves its two directives back out on that one type.
 *
 * fiber.* had a matching entry here before issue #373: the fiber executor's
 * own two directives (fiber.revalidate_freq, fiber.isolate_statics) do not
 * exist on this branch at all any more (they lived on fpm_pool_coop_reval.c
 * / fpm_pool_coop_statics.c, both moved to branch async), so there is no
 * fiber.* namespace left to reject -- an unrecognised directive already
 * fails config parsing on its own, regardless of pool.type. */
static const char *const fpm_pool_fastcgi_rejects[] = {
	"http.",
	"worker.",
	NULL
};

/* Issue #388: a gateway runs no PHP, so nothing that configures PHP has
 * anything to configure. Rejected loudly rather than ignored: a config written
 * for pool.type = http carries the pm. and php_ families on the same section as the gateway,
 * and the gateway cut moves the PHP half to the fastcgi target (see the
 * retirement message for "http" below). An operator who moves the pool and
 * forgets a directive must be told, not left with a proxy that silently has no
 * process manager.
 *
 * http.listen is deliberately NOT here: it is redundant rather than
 * meaningless, and fpm_http_validate_pool() refuses it with a message that says
 * `listen` is the public port. Everything the type keeps -- listen, user/group,
 * chdir, access.*, ping.*, operator.*, http.* -- is simply absent from this
 * list, which is a rejection list and not an allow-list (see the field comment
 * in fpm_pool_type.h). The php_* families are noted to set_directives by
 * fpm_conf.c, so the exact name matches the entry here. */
static const char *const fpm_pool_gateway_rejects[] = {
	"pm",
	"pm.",
	"php_value",
	"php_admin_value",
	"php_flag",
	"php_admin_flag",
	"env",
	"request_terminate_timeout",
	"request_terminate_timeout_track_finished",
	"request_slowlog_timeout",
	"request_slowlog_trace_depth",
	"slowlog",
	"security.limit_extensions",
	"fiber.",
	"worker.",
	NULL
};

/* Both http-direct executors share it: the certificate is read and the reload
 * machinery armed once per pool in the master, before any child forks, and
 * whichever executor the pool resolves to inherits the result (issue #55). */
static int fpm_pool_type_http_direct_init(struct fpm_worker_pool_s *wp)
{
	return fpm_http_direct_tls_init_main(wp);
}

/* The classic executor only (issue #59): the shared segment behind
 * the status page counts what the children do, so it has to exist before the
 * first of them forks. */
static int fpm_pool_type_http_direct_classic_init(struct fpm_worker_pool_s *wp)
{
	if (fpm_pool_type_http_direct_init(wp) < 0) {
		return -1;
	}
	return fpm_http_direct_ops_init_main(wp);
}

/* The worker executor only (issue #333, extended by #339): the shared slots
 * behind fpmng_pool_worker_pending/fpmng_pool_worker_watchers (fpm_pool_type_s.
 * live_gauges below) need to exist before the first child forks, same
 * reasoning as fpm_pool_type_http_direct_classic_init() above for
 * the status page's segment -- and the same reason this is its own function
 * rather than a branch in either of those: an executor variant repeats
 * everything the master must do before fork instead of overriding a shared
 * one.
 *
 * Issue #339: this executor also allocates fpm_http_direct_ops's per-child
 * shm table, the same one the classic executor uses for its status page.
 * That table is not tied to the status page itself -- it is just a per-slot
 * counters/gauges block keyed by scoreboard index -- and the worker executor
 * needs it for the fpmng_pool_worker_* per-slot metrics on operator.metrics_path.
 * operator.status_path stays rejected for this executor (see
 * fpm_http_direct_worker_rejects below); only the underlying table is now
 * shared between both executors. */
static int fpm_pool_type_http_direct_worker_init(struct fpm_worker_pool_s *wp)
{
	if (fpm_pool_type_http_direct_init(wp) < 0) {
		return -1;
	}
	if (fpm_http_direct_ops_init_main(wp) < 0) {
		return -1;
	}
	return fpm_http_direct_worker_metrics_init_main(wp);
}

/* POC, task 073: pool.type = http-direct with pool.executor = worker. Same
 * transport, same listener, same master-side bookkeeping; only the CHILD loop
 * is inverted. Classic http-direct runs one script per request from inside an
 * evhttp callback, so a userland event loop's driver would have to call
 * event_base_loop() on a base that is already looping — libevent 2.1.12-stable
 * returns -1 for that and warns "reentrant invocation". Under this executor
 * the worker instead boots ONE script for its lifetime and that script drives
 * the base itself through fpmng_worker_loop(), so Revolt (and therefore amphp)
 * can suspend. See docs/http-direct-revolt-integration.md.
 *
 * An executor rather than a second pool.type for the same reason "fiber" is
 * (was, on branch async) an executor: the transport is unchanged and only
 * the child's execution model differs. .name stays "http-direct" so
 * diagnostics keep naming the type the operator actually configured. */
/* issue #331: the two directives worker. is a prefix for. FPM_HTTP_DIRECT_REJECTS_COMMON
 * (fpm_http_direct_request.h) rejects the whole "worker." namespace for every
 * http-direct pool, including this one -- these are the exact names carved
 * back out, the same mechanism fpm_pool_type_s.reject_exceptions documents. */
static const char *const fpm_http_direct_worker_accepts[] = {
	"worker.max_pending",
	"worker.request_timeout",
	/* issue #332: worker.send_buffer_limit bounds the per-connection output
	 * queue fpmng_worker_respond_chunk() may build up; meaningless anywhere
	 * else, same reasoning as the two directives above it. */
	"worker.send_buffer_limit",
	/* issue #334: worker.max_memory/worker.max_lifetime, the worker.*
	 * equivalent of supervisor.max_memory/supervisor.max_runtime -- meaningless
	 * anywhere else, same reasoning as the directives above them. */
	"worker.max_memory",
	"worker.max_lifetime",
	/* issue #338: worker.accept_threshold bounds how much of the kernel's
	 * accept queue one worker takes at a time. The classic
	 * executor solves the same problem with a gate of its own (issue #53) that
	 * is not configurable, and no other pool type accepts from a shared socket
	 * this way -- meaningless anywhere else, same reasoning as the directives
	 * above it. */
	"worker.accept_threshold",
	NULL
};

static const struct fpm_pool_type_s fpm_http_direct_worker = {
	.name                         = "http-direct",
	.serves_http11                = 1,
	/* Issue #295, and the one judgement in this file that needed making rather
	 * than reading off #269. Beta, not supported: it is covered by CI on every
	 * PR, it is documented, and nothing is open against its correctness -- but
	 * its long-lived-connection behaviour is the open question of spikes #180
	 * to #183, the cross-worker primitive it is missing is #191, and a spike
	 * that has not run yet may well change a directive. Beta is exactly the
	 * tier that reserves that, and criterion 2 of #269's bar -- measured under
	 * a load resembling use -- is what those spikes will produce.
	 *
	 * Not experimental: it will not disappear. The examples ship against it. */
	.tier                         = FPM_TIER_BETA,
	.requires_listen              = 1,
	.requires_pm                  = 1,
	.serves_requests              = 1,
	.listening_socket_nonblocking = 1,
	.listening_socket_nodelay     = 1,
	.scale_down_drains            = 1,
	.baseline_counter             = "requests",
	/* Both, like everything else here, are repeated rather than inherited: an
	 * executor variant replaces the whole type struct. The renderer has no
	 * effect on this executor yet -- it still rejects operator.status_path
	 * itself (see fpm_http_direct_worker_rejects and issue #59), so no route is
	 * ever registered for it -- but it is the same page from the same shared
	 * counters, so it is set here rather than left for whoever lifts that
	 * reject to discover it missing. */
	.operator_endpoint            = 1,
	.operator_status              = fpm_http_direct_ops_render_status,
	/* Issue #339: the fpmng_pool_worker_* per-slot metrics on operator.metrics_path.
	 * Unlike live_gauges below (a fixed 4-scalar array), these are per-slot
	 * and labeled (pool, slot, reason, type), so they need the same
	 * write-into-a-buffer shape operator_status above already uses rather
	 * than growing that array -- see fpm_pool_type_s's comment on
	 * render_metrics_prometheus. JSON is deliberately not given the same
	 * hook: the metrics page is Prometheus-first, and nothing else on it has
	 * a JSON form either. */
	.render_metrics_prometheus    = fpm_http_direct_ops_render_worker_metrics_prometheus,
	/* Issue #260, same as the base type above: this child owns the accept
	 * socket and narrates its own lifecycle, so its zlog() lines need the
	 * channel back to the master. Repeated rather than inherited, like
	 * everything else in an executor variant. Not .child_php_log_via_master,
	 * for the same reason -- a worker answers requests. */
	.child_logs_via_master        = 1,
	.rejects                      = fpm_http_direct_worker_rejects,
	.reject_exceptions            = fpm_http_direct_worker_accepts,
	.validate                     = fpm_http_direct_worker_validate,
	/* Same master-side TLS setup as the base type above, plus this executor's
	 * OWN shared segment behind live_gauges below (issue #333) and the
	 * fpm_http_direct_ops table behind render_metrics_prometheus above (issue
	 * #339) -- an executor variant replaces the whole type struct rather than
	 * overriding fields of it, so anything the master must do before the
	 * first fork has to be repeated here. Leaving the TLS half out made a TLS
	 * worker pool fork children that found no certificate loaded, exit, and
	 * be respawned forever (issue #55); leaving either metrics half out would
	 * make its gauges always report zero, the exact placeholder these issues
	 * exist to avoid. */
	.init_main                    = fpm_pool_type_http_direct_worker_init,
	.child_main                   = fpm_http_direct_worker_child_main,
	/* issue #333: fpmng_pool_worker_pending / fpmng_pool_worker_watchers,
	 * additive on top of the idle/active/requests this type already reports
	 * through serves_requests above. NOT a stage/duration claim -- see the
	 * comment on fpm_http_direct_worker_rejects for why this type has none of
	 * those to give. */
	.live_gauges                  = fpm_http_direct_worker_live_gauges,
};

/* Used to fill in executor .type pointers from the fiber/async structs, through
 * a lookup that lived in fpm_pool_type_coop.c. Issue #373 cut those structs out
 * to branch async. Issue #388 then retired pool.type = http, the last type that
 * offered a pool.executor list with the fiber/async entries, so there is no
 * list left to fill and no entry left to point at branch async. Kept as a no-op
 * call site rather than deleted so a future variant source has one designated
 * place to hook back in. */
static void fpm_pool_type_install_coop_variants(void)
{
}

/* http-direct ships its own child loop, so it offers its own executor instead
 * of the fiber/async pair — see the comment on fpm_http_direct_worker. */
static const struct fpm_pool_executor_s fpm_http_direct_executors[] = {
	{ .name = "classic", .resolves_to_base = 1 },
	{ .name = "worker", .type = &fpm_http_direct_worker },
	{ .name = NULL }
};

/* Types visible in configuration. gateway is the built-in HTTP proxy (issue
 * #388); fpm_pool_type_resolve() selects the effective variant for the types
 * that offer one. The optimized FastCGI path "fastcgi-ng" used to sit next to
 * "fastcgi" here and was removed in 0.9.0 (issue #376): once fiber/async had
 * left, its only content was the reuses_request_runtime bit, measured at
 * 9.5 us per request (docs/FASTCGI_NG_OPTIMIZATION.md) -- a footnote to
 * "http", which set the same bit. Issue #388 retired "http" itself: it was two
 * things in one section (a pool of PHP workers and the proxy in front of them)
 * and the proxy is now the type it always should have been. Both names are
 * kept as retired names below. */
static const struct fpm_pool_type_s fpm_pool_types[] = {
	{
		.name            = "fastcgi",
		/* issue #340: reachable as an http.route[] target. */
		.serves_fastcgi  = 1,
		/* Issue #295: upstream FPM's own type, unchanged by this project. */
		.tier            = FPM_TIER_SUPPORTED,
		.requires_listen = 1,
		.requires_pm     = 1,
		.serves_requests = 1,
		.baseline_counter = "requests",
		.rejects         = fpm_pool_fastcgi_rejects,
	},
	{
		/* Issue #388: the gateway is the HTTP proxy that used to be welded
		 * onto pool.type = http. It runs no PHP and has no process manager --
		 * .proxy_only carries both facts, so fpm_http.c can tell which
		 * listener it is starting and which routing table it is building
		 * without ever comparing a type name.
		 *
		 * Issue #295: the gateway has shipped since v0.1.0 and CI drives it on
		 * every PR (the gateway-* cells in build-matrix.yml). TLS termination
		 * in front of it is beta, but that is a property of the TLS code and
		 * is announced by it -- see fpm_tls_http.c. */
		.name                   = "gateway",
		.tier                   = FPM_TIER_SUPPORTED,
		/* listen is the PUBLIC HTTP(S) port -- see .proxy_only. */
		.requires_listen        = 1,
		.requires_pm            = 0,
		.serves_requests        = 0,
		.proxy_only             = 1,
		/* Issue #388: the gateway's own operator pages default to /metrics
		 * and /status; on every other type an unset path means "not exposed". */
		.operator_paths_default = 1,
		/* Same socket options as the old http type's gateway processes: the
		 * child answers the client directly, so O_NONBLOCK in the master and
		 * TCP_NODELAY on the listening socket before any fork -- see the two
		 * field comments in fpm_pool_type.h for why not in the child. */
		.listening_socket_nonblocking = 1,
		.listening_socket_nodelay     = 1,
		.operator_endpoint      = 1,
		/* Issue #341: fpmng_gateway_{upstreams_used,upstreams_max,
		 * requests_total,rejected_total}{pool,target} on operator.metrics_path,
		 * one row per http.route[] target -- see fpm_http.h. Write-into-a-buffer
		 * shape, same reason the worker executor's per-slot hook below uses it:
		 * a target label multiplies every series, which does not fit
		 * live_gauges' fixed scalar array. */
		.render_metrics_prometheus = fpm_http_render_metrics_prometheus,
		/* Issue #277/#388: until #390 gives the gateway its own routed-request
		 * counters in shared memory, its baseline counter is the shared
		 * scoreboard's `requests`, exactly as for a serves_requests type --
		 * zero, because no PHP child ever bumps it. The type therefore keeps
		 * .baseline_counter and fpm_operator_page_collect() reads the
		 * scoreboard for a type with this and no .status. */
		.baseline_counter       = "requests",
		.rejects                = fpm_pool_gateway_rejects,
		.validate               = fpm_http_validate_pool,
		.init_main              = fpm_http_init_pool,
	},
	{
		.name                         = "http-direct",
		/* issue #340/#344: a legal target in principle, refused for now. */
		.serves_http11                = 1,
		/* Issue #295: the classic executor, which is what this entry is. The
		 * worker executor is a variant with a tier of its own (beta, see
		 * fpm_http_direct_worker above) -- an executor variant replaces the
		 * whole struct, so the two are classified separately, which is the
		 * point of #269 asking for the worker surfaces one by one. */
		.tier                         = FPM_TIER_SUPPORTED,
		.requires_listen              = 1,
		.requires_pm                  = 1,
		.serves_requests              = 1,
		.listening_socket_nonblocking = 1,
		.listening_socket_nodelay     = 1,
		.scale_down_drains            = 1,
		.baseline_counter             = "requests",
		.executors                    = fpm_http_direct_executors,
		.executors_type_specific      = 1,
		/* Issue #260: the child owns the accept socket, so the child is the only
		 * process that knows it has stopped accepting. Its lifecycle lines --
		 * retiring, the drain deadline, the once-per-child limit NOTICEs -- are
		 * emitted where upstream takes the error_log away, so without the channel
		 * they reach an operator only if the pool also sets catch_workers_output,
		 * a setting whose documented purpose is capturing APPLICATION output.
		 * Deliberately not .child_php_log_via_master: this type serves requests
		 * and has a response to display errors in. */
		.child_logs_via_master        = 1,
		.operator_endpoint            = 1,
		/* Not the generic per-pool summary: this type's own page, moved onto
		 * the operator listener unchanged by issue #275. */
		.operator_status              = fpm_http_direct_ops_render_status,
		.rejects                      = fpm_http_direct_rejects,
		.validate                     = fpm_http_direct_validate,
		.init_main                    = fpm_pool_type_http_direct_classic_init,
		.child_main                   = fpm_http_direct_child_main,
	},
	{
		.name                    = "supervisor",
		/* Issue #295: directives frozen since v0.2.0, failure modes covered by
		 * the fpmng-supervisor-* tests on every PR. */
		.tier                    = FPM_TIER_SUPPORTED,
		.requires_listen         = 0,
		.requires_pm             = 1,	/* pm.* is generated from supervisor.processes; see fpm_pool_supervisor.c */
		.serves_requests         = 0,
		/* Restarts, not runs: a supervised script is meant to be running, so
		 * the number that says something is wrong is how often it had to be
		 * started again. Issue #122 measured 12086 of those per second and
		 * nothing counted them. */
		.baseline_counter        = "restarts",
		.child_logs_via_master   = 1,	/* the whole policy runs in the child; see fpm_child_log.h */
		.child_php_log_via_master = 1,	/* serves no request, so PHP errors have nowhere else to go; issue #124 */
		.publishes_acme_challenges = 1,	/* see the same flag on "cron" below */
		.operator_endpoint       = 1,
		.rejects                 = fpm_pool_supervisor_rejects,
		/* Issue #386: the operator endpoint's directives left the "pm."
		 * namespace, so there is nothing left to carve out of it -- "pm."
		 * here matches only the process-manager directives this type
		 * generates from supervisor.processes. */
		.validate                = fpm_pool_supervisor_validate,
		.init_main               = fpm_pool_supervisor_init_main,
		.child_main              = fpm_pool_supervisor_child_main,
		.status                  = fpm_pool_supervisor_status,
		/* Issue #329: rolling restart across a reload -- see the field's own
		 * doc comment in fpm_pool_type.h and fpm_pool_supervisor.c. */
		.reload_spare_child      = fpm_pool_supervisor_reload_spare_child,
	},
	{
		.name                    = "cron",
		/* Issue #295: as supervisor above, and the ACME process runs on it. */
		.tier                    = FPM_TIER_SUPPORTED,
		.requires_listen         = 0,
		.requires_pm             = 0,	/* validate() always sets pm=static+max_children=1 programmatically */
		.serves_requests         = 0,
		/* Runs, not restarts: a cron script is meant to end, so the number that
		 * says the pool is alive is how many times the schedule fired. */
		.baseline_counter        = "runs",
		.child_logs_via_master   = 1,	/* same as supervisor: fpm_pool_cron_child_main() is where the policy lives */
		.child_php_log_via_master = 1,	/* serves no request, so PHP errors have nowhere else to go; issue #124 */
		/* docs/NOTES.md section 3l puts the dedicated ACME process in a cron
		 * pool, and "supervisor" above carries the same flag: both are
		 * script-running types that serve no request, which is the property
		 * that matters -- the builtins publish into shared memory and a
		 * publisher must not be a process that also answers requests (issue
		 * #48, criterion 7). Whether the client is scheduled or long-running
		 * is issue #49's decision, and this flag does not prejudge it. */
		.publishes_acme_challenges = 1,
		.operator_endpoint       = 1,
		.rejects                 = fpm_pool_cron_rejects,
		/* Issue #386: no operator.* carve-out -- those directives moved out of
		 * "pm.", so "pm." here is the process-manager-only namespace it says
		 * it is (as supervisor above). */
		.validate                = fpm_pool_cron_validate,
		.init_main               = fpm_pool_cron_init_main,
		.child_main              = fpm_pool_cron_child_main,
		.status                  = fpm_pool_cron_status,
		/* Issue #325: cron.stop_signal (default SIGTERM, in which case this is
		 * a no-op) -- fpm_pctl_kill_all() reads it back through this callback
		 * instead of hardcoding SIGTERM for this type the way it still does for
		 * every other non-request-serving type. */
		.stop_signal             = fpm_pool_cron_stop_signal,
	},
	{
		/* Not configurable: created by fpm_operator_endpoint.c, one per distinct
		 * operator listen address, and hidden from fpm_pool_type_get() and
		 * fpm_pool_type_list() by .internal_only. It is an ordinary pool in
		 * every other respect so that it gets a listening socket, a supervised
		 * child and a place in reload without a second supervision path being
		 * invented for it. */
		.name                      = "operator-endpoint",
		/* Issue #295: supported, and therefore silent -- which is what an
		 * internal pool an operator did not write has to be. A tier line
		 * naming a pool nobody configured would be a line with no action
		 * behind it. */
		.tier                      = FPM_TIER_SUPPORTED,
		.internal_only             = 1,
		.requires_listen           = 1,	/* its whole purpose */
		.requires_pm               = 0,	/* validate() sets static + 1 */
		.serves_requests           = 0,
		.reads_foreign_scoreboards = 1,	/* it reports on the pools it serves, not on itself */
		/* Issue #327: rendering a status/metrics page runs ->status() for every
		 * pool it reports on (fpm_operator_pages.c), and cron's status() can
		 * zlog() a "stale" WARNING (fpm_pool_cron_status()) right there, in THIS
		 * child -- not in the reported-on pool's own child. Without this flag
		 * that zlog() call falls all the way back to fpm_stdio_init_child()'s
		 * default (fd closed, zlog_set_fd(-1) -> STDERR_FILENO -> the master's
		 * stdout -> /dev/null; see fpm_child_log.h) and the warning is silently
		 * lost. Same channel supervisor/cron already use for their own
		 * in-child policy messages; this child's messages already carry their
		 * own "[pool %s]" prefix naming the POOL THEY ARE ABOUT, same
		 * convention, so the relayed line still reads correctly even though it
		 * is not this pool's own name. */
		.child_logs_via_master     = 1,
		.rejects                   = fpm_operator_endpoint_rejects,
		.validate                  = fpm_operator_endpoint_validate,
		.child_main                = fpm_operator_endpoint_child_main,
		/* No .status: it has no state of its own worth reporting, so the
		 * collector skips it on that absence rather than detecting it there by
		 * name. */
	},
};

/* Two different questions get answered in the same place at startup and it is
 * worth keeping them apart. fpm_pool_type_check_directives() above asks "does
 * this TYPE support this directive" -- a configuration mistake, identical on
 * every build of this project. This one asks "does this BINARY carry what this
 * type needs" -- the configuration is fine, the executable is not.
 *
 * There is exactly one build where the answer can be no:
 * build/libphp-build.sh links against a distribution's libphp (issue #212) so
 * that `pool.type = fastcgi`, `pool.type = gateway` and `pool.type =
 * http-direct` can ship as a package with no compilation on the user's side. A
 * distribution libphp is built from unpatched php-src, so patches/0006 --
 * which lives inside Zend/ -- is not in it, and
 * zend_signal_use_persistent_handlers() does not exist there. Since issue #388
 * retired pool.type = http (the last type that set the bit) nothing in this
 * tree triggers this refusal; it stays for the next type that needs the patch.
 *
 * Keyed off the capability bit, not off a list of type names. A name list
 * would be a second copy of the same fact and would drift the first time a
 * type gains or loses the behaviour; this way a new type that sets
 * reuses_request_runtime is covered on the day it is written, by the person
 * who set the bit.
 *
 * Why refuse instead of degrading: patch 0006 is invisible when it is missing.
 * Such a pool would start, serve traffic and pass its own tests, while the
 * Zend signal handlers were reinstalled on every request -- upstream
 * behaviour under a name that promises the opposite. That makes every
 * measurement taken on it wrong and says nothing while doing it. The fiber and
 * async executors needed patches 0007/0008 and were handled differently, by
 * being compiled out entirely: on this branch they do not exist at all
 * (issue #373; they live on branch async), and their entries stay rejected
 * by name, so there is nothing to add here for them.
 */
int fpm_pool_type_check_build_support(struct fpm_worker_pool_s *wp, const struct fpm_pool_type_s *type)
{
#ifdef HAVE_FPMNG_PERSISTENT_SIGNALS
	(void) wp;
	(void) type;
	return 0;
#else
	if (!type->reuses_request_runtime) {
		return 0;
	}

	zlog(ZLOG_ALERT, "[pool %s] 'pool.type = %s' is not supported by this binary: it was linked "
		"against a distribution libphp, which does not carry patches/0006 (persistent Zend "
		"signal handlers) -- a pool of this type would run with upstream signal behaviour "
		"without saying so", wp->config->name, type->name);
	zlog(ZLOG_ALERT, "[pool %s] use 'pool.type = fastcgi', 'pool.type = gateway' or "
		"'pool.type = http-direct', which this binary supports in full, or a build from patched "
		"source (build/static-full.sh)", wp->config->name);
	return -1;
#endif
}

/* Is this directive one of the type's declared exceptions to its own reject
 * list? Called with the name as it appears in set_directives, which is not
 * NUL-terminated there, hence the explicit length. */
static int fpm_pool_type_directive_excepted(const struct fpm_pool_type_s *type,
	const char *name, size_t len)
{
	const char *const *allow;

	if (!type->reject_exceptions) {
		return 0;
	}
	for (allow = type->reject_exceptions; *allow; allow++) {
		if (strlen(*allow) == len && !strncmp(*allow, name, len)) {
			return 1;
		}
	}
	return 0;
}

int fpm_pool_type_check_directives(struct fpm_worker_pool_s *wp, const struct fpm_pool_type_s *type)
{
	const char *const *reject;
	char where[160];
	int bad = 0;

	if (!type->rejects || !wp->config->set_directives) {
		return 0;
	}
	/* An executor variant carries the plain type name (fpm_http_direct_worker,
	 * fpm_pool_http_fiber), so without this the message would read "not
	 * supported by pool.type = http-direct" for a directive that the SAME type
	 * accepts under pool.executor = classic. Name the combination that is
	 * actually rejecting it. */
	if (wp->config->executor && *wp->config->executor) {
		snprintf(where, sizeof(where), "pool.type = %s with pool.executor = %s",
			type->name, wp->config->executor);
	} else {
		snprintf(where, sizeof(where), "pool.type = %s", type->name);
	}

	for (reject = type->rejects; *reject; reject++) {
		size_t len = strlen(*reject);

		if (len && (*reject)[len - 1] == '.') {
			/* prefix: "pm." matches every pm.* directive that was actually set */
			const char *p = wp->config->set_directives;
			char needle[128];

			if ((size_t)snprintf(needle, sizeof(needle), ";%s", *reject) >= sizeof(needle)) {
				continue;
			}
			while ((p = strstr(p, needle)) != NULL) {
				const char *end = strchr(p + 1, ';');
				size_t name_len = end ? (size_t)(end - p - 1) : 0;

				if (!fpm_pool_type_directive_excepted(type, p + 1, name_len)) {
					zlog(ZLOG_ALERT, "[pool %s] '%.*s' is not supported by %s",
						wp->config->name, (int) name_len, p + 1, where);
					bad = 1;
				}
				p = end ? end : p + strlen(p);
			}
		} else if (fpm_conf_directive_was_set(wp->config, *reject)
			&& !fpm_pool_type_directive_excepted(type, *reject, len)) {
			zlog(ZLOG_ALERT, "[pool %s] '%s' is not supported by %s",
				wp->config->name, *reject, where);
			bad = 1;
		}
	}

	return bad ? -1 : 0;
}

/* Type names that were real and are not any more, with what replaced each.
 *
 * A retired name gets its own message rather than "unknown pool.type", because
 * the two are different mistakes: an unknown name is a typo, and a retired name
 * is a configuration that used to work. Kept as data, and kept even after the
 * upgrade it names is old news -- a config file outlives the release that
 * broke it. */
static const struct {
	const char *name;
	const char *replacement;
} fpm_pool_types_retired[] = {
	{ "status",
	  "set 'operator.status_path' and 'operator.metrics_path' on the pool you want to watch "
	  "(issue #278); one endpoint per pool replaced the pool that aggregated all of them" },
	{ "fastcgi-ng",
	  "it was removed in 0.9.0 (issue #376): the optimized transport it selected lives on under "
	  "pool.type = fastcgi; use pool.type = fastcgi, or pool.type = http-direct for a pool with "
	  "no web server in front" },
	{ "http",
	  "it was split in two (issue #388): a pool of PHP workers is 'pool.type = fastcgi' and the "
	  "HTTP proxy in front of it is 'pool.type = gateway'. Move the php/pm.* directives to the "
	  "fastcgi section, give the gateway section 'listen = <public port>', and route to the "
	  "workers explicitly, e.g. 'http.route[<pool>] = /'" },
};

/* NULL when the name is not a retired one. */
const char *fpm_pool_type_retired(const char *name)
{
	size_t i;

	if (!name || !*name) {
		return NULL;
	}

	for (i = 0; i < sizeof(fpm_pool_types_retired) / sizeof(fpm_pool_types_retired[0]); i++) {
		if (!strcmp(fpm_pool_types_retired[i].name, name)) {
			return fpm_pool_types_retired[i].replacement;
		}
	}

	return NULL;
}

#define FPM_POOL_TYPE_COUNT (sizeof(fpm_pool_types) / sizeof(fpm_pool_types[0]))
#define FPM_POOL_TYPE_DEFAULT (&fpm_pool_types[0])

const struct fpm_pool_type_s *fpm_pool_type_get(const char *name)
{
	size_t i;

	/* Every path into this file starts here -- see the comment on the loop
	 * below -- so this is the one place that must run before the executor
	 * tables are read anywhere. */
	fpm_pool_type_install_coop_variants();

	if (!name || !*name) {
		return FPM_POOL_TYPE_DEFAULT;
	}

	/* Explicit "fcgi" was accepted before the name changed to "fastcgi". */
	if (!strcmp(name, "fcgi")) {
		return FPM_POOL_TYPE_DEFAULT;
	}

	/* Internal-only types are found here too. They have to be: every pool goes
	 * through fpm_pool_type_of() -> resolve() -> get(), including the ones
	 * fpm-ng creates for itself, and a NULL here would silently fall back to
	 * the default type (fastcgi) and run an operator listener as a FastCGI
	 * pool. Keeping such a type out of a CONFIGURATION is a separate question,
	 * answered where a configuration is read -- see
	 * fpm_pool_type_check_configurable(). */
	for (i = 0; i < FPM_POOL_TYPE_COUNT; i++) {
		if (!strcmp(fpm_pool_types[i].name, name)) {
			return &fpm_pool_types[i];
		}
	}

	return NULL;
}

/* Was this type named by the configuration rather than created by fpm-ng?
 * An internal-only type that a pool section asked for is not a type this
 * configuration may have: the pool behind it is an implementation detail of
 * some other pool, with settings its owner chose, so a section naming it would
 * get a pool it cannot configure and did not ask for. Reported as an unknown
 * type, which is what it is from a configuration's point of view. */
int fpm_pool_type_check_configurable(struct fpm_worker_pool_s *wp, const struct fpm_pool_type_s *type)
{
	char known[256];

	if (!type->internal_only || !fpm_conf_directive_was_set(wp->config, "pool.type")) {
		return 0;
	}

	fpm_pool_type_list(known, sizeof(known));
	zlog(ZLOG_ALERT, "[pool %s] unknown pool.type '%s'; known types: %s",
		wp->config->name, wp->config->type, known);

	return -1;
}

void fpm_pool_type_list(char *buf, size_t len)
{
	size_t i, off = 0;

	if (!len) {
		return;
	}
	buf[0] = '\0';

	for (i = 0; i < FPM_POOL_TYPE_COUNT && off + 1 < len; i++) {
		int n;

		if (fpm_pool_types[i].internal_only) {
			continue;
		}
		n = snprintf(buf + off, len - off, "%s%s",
			off ? ", " : "", fpm_pool_types[i].name);
		if (n < 0 || (size_t)n >= len - off) {
			break;
		}
		off += (size_t)n;
	}
}

static const struct fpm_pool_executor_s *fpm_pool_executor_find(
	const struct fpm_pool_type_s *type, const char *name)
{
	const struct fpm_pool_executor_s *e;

	if (!type->executors) {
		return NULL;
	}
	for (e = type->executors; e->name; e++) {
		if (!strcmp(e->name, name)) {
			return e;
		}
	}

	return NULL;
}

/* "classic, fiber, async" for an error message. sep is ", " when the list is
 * read as a set of known names and " or " when it is read as the only
 * acceptable choices; see fpm_pool_type_s.executors_type_specific. */
static void fpm_pool_executor_list(const struct fpm_pool_type_s *type,
	const char *sep, char *buf, size_t len)
{
	const struct fpm_pool_executor_s *e;
	size_t off = 0;

	if (!len) {
		return;
	}
	buf[0] = '\0';

	for (e = type->executors; e && e->name && off + 1 < len; e++) {
		int n = snprintf(buf + off, len - off, "%s%s", off ? sep : "", e->name);

		if (n < 0 || (size_t)n >= len - off) {
			break;
		}
		off += (size_t)n;
	}
}

const struct fpm_pool_type_s *fpm_pool_type_resolve(struct fpm_worker_pool_s *wp)
{
	const struct fpm_pool_type_s *type = fpm_pool_type_get(wp->config->type);
	const struct fpm_pool_executor_s *e;
	const char *executor = wp->config->executor;

	if (!type) {
		return NULL;
	}
	if (!executor || !*executor) {
		return type;
	}

	e = fpm_pool_executor_find(type, executor);
	if (!e) {
		return NULL;
	}
	if (e->resolves_to_base) {
		return type;
	}

	/* e->type == NULL means the executor is not in this build. Unreachable in
	 * practice: fpm_pool_type_validate_executor() already refuses that
	 * combination before resolve() is ever called (see fpm_conf.c). Kept for
	 * defensive symmetry. */
	return e->type;
}

int fpm_pool_type_validate_executor(struct fpm_worker_pool_s *wp)
{
	const struct fpm_pool_type_s *type = fpm_pool_type_get(wp->config->type);
	const struct fpm_pool_executor_s *e;
	const char *executor = wp->config->executor;
	char known[160];

	if (!type || !executor || !*executor) {
		return 0;
	}
	if (!type->executors) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor is not supported by pool.type = %s",
			wp->config->name, type->name);
		return -1;
	}

	e = fpm_pool_executor_find(type, executor);
	if (!e) {
		if (type->executors_type_specific) {
			fpm_pool_executor_list(type, " or ", known, sizeof(known));
			zlog(ZLOG_ALERT, "[pool %s] pool.type = %s supports only pool.executor = %s",
				wp->config->name, type->name, known);
		} else {
			fpm_pool_executor_list(type, ", ", known, sizeof(known));
			zlog(ZLOG_ALERT, "[pool %s] unknown pool.executor '%s'; known executors: %s",
				wp->config->name, executor, known);
		}
		return -1;
	}

	if (!e->resolves_to_base && !e->type) {
		zlog(ZLOG_ALERT, "[pool %s] pool.executor = %s is not on this branch: it lives on "
			"branch async of the repository",
			wp->config->name, e->name);
		return -1;
	}

	return 0;
}

const struct fpm_pool_type_s *fpm_pool_type_of(struct fpm_worker_pool_s *wp)
{
	const struct fpm_pool_type_s *type = fpm_pool_type_resolve(wp);

	return type ? type : FPM_POOL_TYPE_DEFAULT;
}

/* TCP_NODELAY for a type that declares it, on the socket the master owns. Why
 * here and not in the child: see listening_socket_nodelay in fpm_pool_type.h.
 *
 * A socket that is not AF_INET/AF_INET6 has no Nagle to turn off, and asking
 * for the option there would fail with ENOPROTOOPT -- so the family decides
 * whether there is anything to do, rather than the error being swallowed. */
static int fpm_pool_type_set_nodelay(struct fpm_worker_pool_s *wp)
{
	struct sockaddr_storage address;
	socklen_t address_len = sizeof(address);
	int on = 1;

	if (getsockname(wp->listening_socket, (struct sockaddr *) &address, &address_len) < 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] failed to read the listening socket's address",
			wp->config->name);
		return -1;
	}
	if (address.ss_family != AF_INET && address.ss_family != AF_INET6) {
		return 0;
	}
	if (setsockopt(wp->listening_socket, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] failed to set TCP_NODELAY on the listening socket",
			wp->config->name);
		return -1;
	}

	return 0;
}

int fpm_pool_type_prepare_listening_socket(struct fpm_worker_pool_s *wp)
{
	const struct fpm_pool_type_s *type = fpm_pool_type_of(wp);
	int flags;
	int desired;

	if (!type->requires_listen) {
		return 0;
	}

	/* The socket is an open file description shared by the master and every
	 * child. Set its status once here, while the master owns the configuration,
	 * and repeat this after exec-based reloads to normalize inherited sockets. */
	flags = fcntl(wp->listening_socket, F_GETFL);
	if (flags < 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] failed to read listening socket flags",
			wp->config->name);
		return -1;
	}

	if (type->listening_socket_nodelay && fpm_pool_type_set_nodelay(wp) < 0) {
		return -1;
	}

	desired = type->listening_socket_nonblocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
	if (desired == flags) {
		return 0;
	}

	if (fcntl(wp->listening_socket, F_SETFL, desired) < 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] failed to set listening socket flags",
			wp->config->name);
		return -1;
	}

	return 0;
}


/* The child must find its pool, and when respawned from the event loop the
 * pointer is lost in fpm_children.c. The scoreboard is per pool and the child
 * receives its own in fpm_scoreboard_init_child(), so matching is enough —
 * without touching fpm_children.c. Called once when the child starts. */
struct fpm_worker_pool_s *fpm_pool_type_current_pool(void)
{
	struct fpm_scoreboard_s *sb = fpm_scoreboard_get();
	struct fpm_worker_pool_s *wp;

	if (!sb) {
		return NULL;
	}

	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		if (wp->scoreboard == sb) {
			return wp;
		}
	}

	return NULL;
}

/* Issue #295. Spelled the way the operator wrote it, not the way the code
 * resolved it: "pool.executor = worker" and not the name of the variant
 * struct, because the line has to be findable in the configuration the
 * operator is reading while they read the log. */
void fpm_pool_type_announce_tier(struct fpm_worker_pool_s *wp)
{
	const struct fpm_pool_type_s *type = fpm_pool_type_of(wp);
	const char *executor = wp->config->executor;
	char subject[128];

	if (!type) {
		return;
	}

	if (executor && *executor && strcmp(executor, "classic") != 0) {
		snprintf(subject, sizeof(subject), "pool.type = %s with pool.executor = %s",
			type->name, executor);
	} else {
		snprintf(subject, sizeof(subject), "pool.type = %s", type->name);
	}

	fpm_tier_announce(type->tier, wp->config->name, subject);
}
