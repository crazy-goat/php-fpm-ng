/* fpm-ng: pool types.
 *
 * Adding a new pool type means one new file plus one line in the
 * fpm_pool_types[] table in fpm_pool_type.c. Nothing else — in particular,
 * neither validation logic in fpm_conf.c nor fpm_children.c may know about a
 * type. Therefore a type declares configuration requirements as DATA (the
 * fields below), not as code scattered through validation.
 */

#ifndef FPM_POOL_TYPE_H
#define FPM_POOL_TYPE_H 1

#include <time.h>

#include "fpm_tier.h"

struct fpm_worker_pool_s;
struct fpm_operator_reply_s;
struct fpm_operator_buf_s;

/* State for a pool that does NOT handle requests (serves_requests = 0). For
 * fcgi/http pools (serves_requests = 1), the data shape is different
 * (idle/active/requests from the scoreboard) and this enum does not apply —
 * see fpm_operator_pages.c. */
enum fpm_pool_state_e {
	FPM_POOL_STATE_RUNNING = 0,	/* currently running a script */
	FPM_POOL_STATE_BACKOFF,		/* waiting during backoff before the next attempt (supervisor) */
	FPM_POOL_STATE_GAVE_UP,		/* gave up permanently, real failure (supervisor.restart_max) */
	FPM_POOL_STATE_FINISHED,	/* exited as planned, not a failure (restart=never/on-failure+success) */
	FPM_POOL_STATE_IDLE		/* doing nothing now, waiting for the next due time (cron between runs) */
};

/* Filled by fpm_pool_type_s.status() for types with serves_requests = 0.
 * Exactly the fields the operator pages show — see docs/NOTES.md section 3u. */
struct fpm_pool_status_s {
	enum fpm_pool_state_e state;
	time_t last_start;		/* epoch, 0 = never started */
	int last_exit_code;
	unsigned consecutive_failures;	/* consecutive exit_code != 0 */
	time_t next_run;		/* cron only: next due time from the schedule */
	time_t backoff_until;		/* supervisor only: end of current backoff */
	unsigned long baseline;		/* the type's baseline counter, see
					 * fpm_pool_type_s.baseline_counter */
	unsigned has_last_exit_code:1;
	unsigned has_next_run:1;
	unsigned has_backoff_until:1;

	/* cron.expect_within (issue #327), cron only. has_expect_within = the
	 * directive is set and the schedule parsed -- true from the pool's very
	 * first render, independent of whether a run has happened yet, so the
	 * series is never absent while the directive is configured (see
	 * fpm_pool_cron_status()). `stale` is meaningful only once has_expect_within
	 * is set; with no run yet to compare against it simply reads false (not
	 * "unknown"), so a renderer must still gate on has_expect_within before
	 * reading `stale`, but must not read "false" as "there has been a run". */
	unsigned has_expect_within:1;
	unsigned stale:1;		/* 1 = a scheduled run is overdue past cron.expect_within */
	time_t stale_since;		/* the schedule's due time this is stale against; 0 if not stale */

	/* fpmng_supervisor_heartbeat() (issue #327), supervisor only.
	 * has_heartbeat = the script has called it at least once in this process's
	 * lifetime (shared memory, so it also survives this process being
	 * respawned -- the shared struct is keyed by pool, not by process, see
	 * fpm_pool_supervisor_shared_for()). last_heartbeat is the raw timestamp;
	 * the age an operator cares about ("stuck since...") is
	 * FPM_NOW() - last_heartbeat, computed where it is rendered rather than
	 * stored, exactly like next_run/uptime. FPM_NOW() on both sides, never
	 * time(NULL) on one of them: see issue #396 and fpm_debug_clock.h.
	 *
	 * NOTE: the shared struct this is stored in is allocated once per POOL, not
	 * per child, so with supervisor.processes > 1 every child of the pool
	 * shares and overwrites the same has_heartbeat/last_heartbeat pair --
	 * "last call from any child in this pool", not "per child". Tracking it per
	 * child would need a per-child key into shared memory that does not exist
	 * today; see the supervisor heartbeat granularity follow-up issue. */
	unsigned has_heartbeat:1;
	time_t last_heartbeat;
};

/* One extra Prometheus/JSON gauge from fpm_pool_type_s.live_gauges() below
 * (issue #333). json_key doubles as the Prometheus series' suffix: the
 * rendered name is fpmng_pool_<json_key>{pool="..."}, and the same string is
 * the JSON object's key, so the two formats can never drift apart the way two
 * separately-spelled names could. */
struct fpm_pool_live_gauge_s {
	const char *json_key;
	const char *help;
	long value;
};

/* fpm_pool_type_s.live_gauges() fills at most this many entries of the out[]
 * array per call. Fixed rather than a dynamic count: every implementation
 * today (fpm_http_direct_worker_live_gauges(), issue #333) fills 2, and a
 * type that ever needs more than a handful of ad hoc gauges belongs in
 * fpm_metrics.h's application-metrics path instead, not in the operator
 * page's fixed per-pool row. */
#define FPM_POOL_LIVE_GAUGES_MAX 4

/* One pool.executor value accepted by a pool type, and what it resolves to.
 * The list a type carries is complete in every build: an executor behind a
 * configure flag that is off keeps its entry, with .type NULL and .build_flag
 * naming the flag, so the binary can still tell "not built" apart from
 * "no such executor". */
struct fpm_pool_executor_s {
	const char *name;

	/* Type variant this executor resolves to, or NULL when the executor is
	 * known but absent from this build (then .build_flag is set). Ignored
	 * when .resolves_to_base is set. */
	const struct fpm_pool_type_s *type;

	/* configure flag that would provide this executor; set only when .type is
	 * NULL because the flag was off. */
	const char *build_flag;

	/* This executor is the type's own default behaviour, so it resolves to
	 * the base type rather than to a variant. True for "classic". */
	unsigned resolves_to_base:1;
};

struct fpm_pool_type_s {
	const char *name;

	/* What this type promises, and therefore what it withholds (issue #269,
	 * implemented in #295). Data on the type like everything else here, so
	 * that nothing anywhere compares a type NAME to decide how loudly to
	 * announce it. The default, FPM_TIER_EXPERIMENTAL, is 0: a type added
	 * without a thought about this field announces itself as the least
	 * promised of the three, which is the honest reading of code nobody has
	 * classified. Every type below states its tier explicitly all the same,
	 * so that the value is a decision someone made rather than a field left
	 * alone. Announced once per pool at startup by fpm_run(); see
	 * fpm_tier.h. */
	enum fpm_tier tier;

	/* Configuration requirements — read by fpm_conf.c, which does not know types. */
	unsigned requires_listen:1;		/* pool must have a listening address */
	unsigned requires_pm:1;			/* pool must have meaningful pm/pm.max_children */
	unsigned serves_requests:1;		/* counted in the request scoreboard */

	/* Issue #388: this type IS an HTTP proxy and nothing else. It runs no
	 * PHP and has no process manager, so:
	 *   - `listen` is the PUBLIC HTTP(S) port the type's own child serves,
	 *     not a FastCGI socket. The child accepts on the master's listening
	 *     socket directly (fpm_http.c) and `http.listen` is refused as
	 *     redundant.
	 *   - routing is exactly http.route[]: there is no implicit "own pool"
	 *     target at "/" (the target-0 row #340 adds for pool.type = http),
	 *     at least one route is required at startup, and a request matching
	 *     none is a local 404, never a forward.
	 *   - the process count comes from http.gateways alone; there are no
	 *     pm.max_children children to tie it to.
	 * This is data on the type, not a name comparison in fpm_http.c: the
	 * proxy machinery asks the flag, the way every other per-type decision
	 * here is asked. */
	unsigned proxy_only:1;

	/* Issue #388: operator.metrics_path and operator.status_path default to
	 * "/metrics" and "/status" on this type rather than to "off". Only the
	 * gateway sets it: on it the operator listener is the one place its own
	 * (and, since #389, every target's) pages can be scraped, so binding it
	 * without being asked is the useful default. On every other type an
	 * unset path means the pool is not exposed. */
	unsigned operator_paths_default:1;

	/* What a pool of this type speaks on its own listener, asked by the HTTP
	 * gateway's router when http.route[] names it as a target (issue #340).
	 * Data on the type, not a name comparison in fpm_http.c, for the same
	 * reason operator_endpoint below is: nothing outside this file should be
	 * able to answer "is this a fastcgi pool?" by spelling the name.
	 *
	 * Both are 0 on every type that has no request listener at all (cron,
	 * supervisor) and on `http` itself -- a gateway in front of a gateway is
	 * a nested proxy, and the router refuses it. serves_http11 is set on
	 * http-direct, whose pools ARE reachable in principle; the gateway
	 * refuses them today with "not yet supported (see #344)" rather than as a
	 * permanent rejection, and that bit is how it tells that case apart from
	 * a type that could never be a target. */
	unsigned serves_fastcgi:1;
	unsigned serves_http11:1;

	/* This type, in its OWN child, reads another pool's FOREIGN scoreboard
	 * (the operator endpoint: idle/active/requests of the serves_requests = 1
	 * pool it reports on). See fpm_children.c:
	 * fpm_child_resources_use() normally releases (munmaps) scoreboards for ALL
	 * pools except its own immediately after fork, as memory hygiene — safe because
	 * no type had read a foreign scoreboard until now. This flag disables the
	 * release ONLY for a child of THIS type (checked through
	 * fpm_pool_type_of(child->wp) in fpm_children.c); every other pool in the same
	 * configuration (including ordinary fcgi/http) still releases foreign
	 * scoreboards exactly as today, whether or not an operator listener exists
	 * anywhere in the configuration. See docs/NOTES.md 3u. */
	unsigned reads_foreign_scoreboards:1;

	/* This type's own policy runs in the CHILD (supervisor backoff, restart_max,
	 * cron timeouts, "cannot open script"), so the messages an operator needs
	 * are emitted where upstream FPM assumes nothing worth logging happens and
	 * takes the error_log away — see fpm_child_log.h. Setting this gives the
	 * type's children a log channel back to the master; every zlog() in such a
	 * child then lands in error_log at its own level. Costs one socketpair per
	 * pool of this type, and nothing at all for any other pool.
	 *
	 * This is about the pool type's own zlog() lines, nothing else. PHP's own
	 * diagnostics are a separate bit below, because they rest on a narrower
	 * premise (issue #260). */
	unsigned child_logs_via_master:1;

	/* PHP's OWN diagnostics -- errors, warnings, uncaught exceptions,
	 * error_log() from the script -- go through that channel too, and the
	 * child's INI defaults become log_errors = 1, display_errors = 0,
	 * html_errors = 0 (issue #124, fpm_child_php_log.h). Requires
	 * child_logs_via_master, since the channel is what it writes into.
	 *
	 * A separate bit from the one above, and the difference is the premise.
	 * The channel is for "the policy runs in the child", which is true of any
	 * type that has its own child loop. This one is for "the child has nowhere
	 * else to put a PHP error": a supervisor or cron child serves no request,
	 * so it has neither a response nor a front end's FastCGI stderr, and taking
	 * display_errors away from it costs nothing an operator asked for. A
	 * request-serving type fails that premise -- an http-direct child has a
	 * response to display errors in, and display_errors from php.ini means
	 * there what it has always meant -- so it takes the channel (issue #260)
	 * and leaves this alone. */
	unsigned child_php_log_via_master:1;

	/* A child of this type may publish HTTP-01 challenge answers, so it gets
	 * the fpmng_acme_challenge_* builtins (fpm_acme_challenge.h). Set for the
	 * script-running types that serve no request ("cron", where docs/NOTES.md
	 * section 3l puts the dedicated ACME process, and "supervisor"). Data
	 * rather than a name comparison in fpm_pool_script.c, and
	 * deliberately not set for request-serving types: a gateway must never
	 * execute the ACME client (issue #48, criterion 7). */
	unsigned publishes_acme_challenges:1;

	/* This type's own SIGUSR1 handler drains a single child instead of
	 * treating it as a log-reopen: stop accepting, finish what is already
	 * open, exit on its own within http.read_timeout (issue #65). Data for
	 * fpm_pctl_kill_idle_child() (fpm_process_ctl.c), read in the MASTER, not
	 * the child: a master-driven scale-down of a type that sets this bit
	 * reaches that drain trigger instead of the pool-wide-shutdown SIGQUIT,
	 * which this type's "stopping" gate treats as fast-exit-now and answers
	 * by dropping every connection the child still holds (issue #310). Set
	 * for both "http-direct" struct literals below (classic and the worker
	 * executor) since both install the same SIGUSR1 handler; unset (the
	 * default) for every type with no such handler, where SIGQUIT staying the
	 * scale-down signal is unchanged. */
	unsigned scale_down_drains:1;

	/* Status flags are established on the master-side listening socket before
	 * children are forked. The open file description is shared by the master
	 * and its children, so a child must not change this after fork. */
	unsigned listening_socket_nonblocking:1;

	/* TCP_NODELAY on the same master-side socket, for a type whose children
	 * speak to the client directly (issue #244). FPM's own listener code never
	 * set it: it was written for FastCGI, where the peer is a web server on the
	 * same host and Nagle costs nothing. For a type that answers the client, it
	 * costs the trailing partial segment of every response over a few kilobytes
	 * a wait for the peer's delayed ACK -- 43 ms on the poligon, against 0.3 ms
	 * without.
	 *
	 * In the master and before the fork, not in the child, because Linux copies
	 * the listening socket's options onto a connection when the handshake
	 * completes and not when accept() returns it. A child that sets the option
	 * during its own start-up therefore leaves whatever was already sitting in
	 * the accept queue on the old setting -- which is exactly the race that a
	 * first attempt at this produced, as a test that stalled on some runs and
	 * not others.
	 *
	 * A no-op on a unix socket, where there is no Nagle: the flag is applied
	 * only to AF_INET/AF_INET6. */
	unsigned listening_socket_nodelay:1;

	/* Every pool.executor value this type accepts, and the type variant each
	 * one resolves to. Terminated by an entry with .name == NULL. NULL = the
	 * type accepts no pool.executor at all. Data, not a name comparison in
	 * fpm_pool_type_resolve(): a type that ships an execution model declares
	 * it here instead of teaching resolve() about another name. */
	const struct fpm_pool_executor_s *executors;

	/* The executor list above is specific to this transport rather than the
	 * general set every request-serving type offers. Diagnostics only: an
	 * unaccepted executor is reported by enumerating the list ("supports only
	 * pool.executor = classic or worker") instead of calling the name unknown,
	 * because for such a type the name may well be valid elsewhere. */
	unsigned executors_type_specific:1;

	/* This type has nothing in front of it that could answer an operator's
	 * scrape, so a pool of it serves its own stats and metrics from a small
	 * HTTP listener of its own -- see fpm_operator_endpoint.h and issue #273.
	 * Set for cron, supervisor, http and http-direct.
	 *
	 * Off for fastcgi, where it changes what pm.status_path
	 * means: with the flag off the path keeps its upstream meaning, answered on
	 * the pool's own FastCGI socket by whatever web server is already in front
	 * of it, which on those types is exactly what an operator has (#273,
	 * point 2). The flag is therefore not cosmetic and not a default -- adding
	 * it to a type moves that type's status endpoint onto another socket.
	 *
	 * Data rather than a name comparison in fpm_conf.c, which must not learn
	 * the name of a pool type. */
	unsigned operator_endpoint:1;

	/* How this type renders its status page on the operator endpoint. NULL is
	 * the common case and means the generic per-pool JSON that
	 * fpm_operator_page_render_json() produces for every type.
	 *
	 * It exists for http-direct, whose page is not that summary: it carries
	 * per-connection counters and, on "?full", a row per child (issue #64), and
	 * three of this repo's tests have nothing else to observe retirement, idle
	 * signalling and accept distribution with. Issue #275 moved that page from
	 * the pool's own listener onto the operator endpoint without changing a byte
	 * of it, and this callback is how: the endpoint asks the type for its body
	 * instead of knowing which types have an unusual one.
	 *
	 * Called in the operator endpoint's own child, which is not a child of the
	 * pool being reported on. Everything it reads must therefore be shared
	 * memory or configuration -- never the heap of the pool's children.
	 *
	 * query is the request's query string with the '?' removed, empty when there
	 * was none; use fpm_operator_http_has_flag() on it. The callback fills
	 * reply->body and may set reply->content_type; the endpoint marks the reply
	 * handled. */
	void (*operator_status)(struct fpm_worker_pool_s *wp, const char *query,
		struct fpm_operator_reply_s *reply);

	/* Extra Prometheus lines appended to the operator endpoint's metrics page,
	 * after the generic per-pool lines and after live_gauges above. NULL is the
	 * common case (nothing extra). Unlike live_gauges' fixed array of scalar
	 * gauges, this callback writes its own lines straight into the buffer --
	 * the same shape operator_status above already uses for the status page --
	 * because what it has to report is per-slot (issue #339: pool.executor =
	 * worker's fpmng_pool_worker_queued{pool,slot} and friends), and a slot
	 * label multiplies every gauge by pm.max_children, which is exactly the
	 * "more than a handful" case FPM_POOL_LIVE_GAUGES_MAX's own comment says
	 * does not belong in that array.
	 *
	 * Called in the operator endpoint's own child, same constraint as
	 * operator_status: shared memory and configuration only. JSON is
	 * deliberately not given the same hook -- the metrics page is this
	 * project's Prometheus-first surface (docs/operator-endpoint.md), and the
	 * JSON status page already carries this type's pool-wide numbers through
	 * live_gauges. */
	void (*render_metrics_prometheus)(struct fpm_worker_pool_s *wp, struct fpm_operator_buf_s *b);

	/* This type exists to be created by fpm-ng itself and cannot be named in a
	 * configuration: fpm_pool_type_get() will not return it and
	 * fpm_pool_type_list() does not mention it. Set for the internal listener
	 * pool behind the operator endpoint, which is an implementation detail of
	 * the pools it serves rather than something an operator configures. */
	unsigned internal_only:1;

	/* Directives unsupported by this type. NULL-terminated, may be NULL.
	 * A REJECTION list, not an allow-list — a new directive is allowed everywhere
	 * by default, so an omission does not break backward compatibility.
	 * A name ending in a dot works as a prefix: "pm." matches all pm.*. */
	const char *const *rejects;

	/* Directives this type accepts even though .rejects matches them.
	 * NULL-terminated, may be NULL. Exact names only -- a prefix here would be
	 * a second pattern language arguing with the first one.
	 *
	 * It exists because a type sometimes rejects a whole namespace while
	 * accepting a few exact names in it: pool.executor = worker rejects the
	 * whole "worker." namespace, then carves its own directives back out.
	 * (Before issue #386 it also carved the operator endpoint's directives out
	 * of the "pm." namespace that cron and supervisor reject; the rename to
	 * "operator." removed that need.)
	 *
	 * Not by dropping the prefix and enumerating every real directive in it:
	 * that is the enumeration-versus-pattern mistake build/prepare.sh:75-79
	 * documents, and a directive added later would silently become legal on a
	 * pool that rejects its namespace. An exception must be added deliberately;
	 * a new directive must not become one by omission. */
	const char *const *reject_exceptions;

	/* Type-specific checks; NULL = none. Returns 0 or -1. */
	int (*validate)(struct fpm_worker_pool_s *wp);

	/* Master side, after validation and before child fork; NULL = none. */
	int (*init_main)(struct fpm_worker_pool_s *wp);

	/* What the child does instead of the accept loop; NULL = ordinary FastCGI
	 * loop. Does not return. */
	void (*child_main)(struct fpm_worker_pool_s *wp);

	/* How this type appears on the operator pages when serves_requests = 0.
	 * NULL for serves_requests = 1 types (they have idle/active/requests from the
	 * scoreboard, read directly by fpm_operator_pages.c) and for types without a
	 * meaningful state to show. Called from ANOTHER process (the operator
	 * endpoint's child), so it must read only shared memory/configuration, never
	 * process-local memory. */
	void (*status)(struct fpm_worker_pool_s *wp, struct fpm_pool_status_s *out);

	/* The signal fpm_pctl_kill_all() (fpm_process_ctl.c) sends to a child of
	 * this type instead of the hardcoded SIGTERM it otherwise uses for every
	 * non-request-serving pool on shutdown/reload -- NULL means "no override",
	 * i.e. today's plain SIGTERM. Never consulted for the final SIGKILL
	 * escalation (that stays lethal and unconditional) or for the SIGQUIT a
	 * request-serving pool gets first; only for the point where the master had
	 * already decided this child gets SIGTERM. Issue #325: cron.stop_signal is
	 * the first (and, for now, only) directive that gives a per-pool answer
	 * here, through fpm_pool_cron_stop_signal() -- see fpm_pool_cron.c. Data,
	 * not a name comparison in fpm_process_ctl.c, which must not learn which
	 * type "cron" is. */
	int (*stop_signal)(struct fpm_worker_pool_s *wp);

	/* Issue #329: called from fpm_pctl_kill_all() (fpm_process_ctl.c) exactly
	 * once per pool, only on the FIRST signal pass of a RELOADING transition
	 * (fpm_state == FPM_PCTL_STATE_RELOADING and fpm_signal_sent == 0 there --
	 * see the call site) and only for a type that sets this. NULL = no
	 * override, i.e. today's behavior: every child of the pool is signalled in
	 * the same pass, same as any other reload/shutdown.
	 *
	 * A type that sets this may detach up to ONE of wp's children from the
	 * pool's own pm.*-counted bookkeeping (fpm_children_detach_oldest(),
	 * fpm_children_extra.h) instead of leaving it for the signal loop that
	 * follows immediately after this call returns. The master's "wait for
	 * zero running children across every pool, then execvp()" reload gate
	 * (fpm_pctl_action_next()) then does not wait on that child -- it survives
	 * the execvp() still running, unsignalled, the OLD generation's code,
	 * until the type's own post-reload logic decides to retire it.
	 *
	 * Exists because a php-fpm-ng reload is execvp()-based (docs/NOTES.md
	 * 2562-2566: a real per-pool selective reload is issue #330, "several
	 * weeks of work", not this) and fpm_shm_alloc()'s MAP_ANONYMOUS mapping
	 * does not survive execve() (NOTES.md 3p) -- so without this, a pool with
	 * more than one copy of the same long-running script
	 * (supervisor.processes >= 2) goes through a whole reload with ZERO live
	 * copies at once, however briefly. Implemented only for "supervisor" (see
	 * fpm_pool_supervisor_reload_spare_child()); every other type leaves this
	 * NULL and reloads exactly as before. Data, not a name comparison here or
	 * in fpm_process_ctl.c, which must not learn which type "supervisor" is. */
	void (*reload_spare_child)(struct fpm_worker_pool_s *wp);

	/* The one counter this type reports whether or not the pool's script ever
	 * touches fpm_metric_*() (issue #277). The answer to "is this pool doing
	 * anything", which before this had no answer on a pool whose code registers
	 * no series of its own -- and on supervisor could not have one, since a
	 * supervised script is not the shape that calls fpm_metric_inc().
	 *
	 * This is the SHORT name: the JSON key is it, and the Prometheus series is
	 * fpmng_pool_<this>_total. One field rather than two because the two
	 * spellings have to agree, and a pair invites them not to.
	 *
	 * What it counts is the type's own idea of an invocation: a request for the
	 * types that serve requests, a run for cron, a restart for supervisor. Where
	 * the number comes from is not here -- a type that serves requests has it in
	 * the scoreboard, the others fill fpm_pool_status_s.baseline from their own
	 * shared memory. NULL = this type counts nothing (today: "status" itself,
	 * which reports on others and not on itself).
	 *
	 * MONOTONIC, and that is a requirement rather than a description: a counter
	 * that resets when a child is recycled is worse than no counter, because a
	 * rate() over it reads as a dip rather than as a gap. Everything feeding it
	 * lives in shared memory the master allocated, so a child dying and being
	 * respawned does not touch it. */
	const char *baseline_counter;

	/* Issue #390: the VALUE of .baseline_counter for a serves_requests = 0 type
	 * whose counter does not live in the shared scoreboard. The alternative --
	 * moving the number into .status()'s fpm_pool_status_s.baseline -- would
	 * work for the gateway only by giving it a state block it does not have, and
	 * fpm_operator_pages.c would then have to render that zeroed state as a
	 * measured one. This callback reports the counter and nothing else.
	 *
	 * NULL = read the shared scoreboard's `requests` (a serves_requests type,
	 * or a types-less count that never happens). Only the gateway sets it today:
	 * fpm_http_gateway_baseline_requests(). Called from the operator endpoint's
	 * own child, so -- same contract as .status() and .live_gauges() -- shared
	 * memory and configuration only. */
	unsigned long (*baseline)(struct fpm_worker_pool_s *wp);

	/* Extra per-pool gauges the fixed row shape above has no field for --
	 * issue #333, the worker executor's currently-pending and watcher counts.
	 * Orthogonal to serves_requests: unlike .status(), which is the WHOLE
	 * shape for a serves_requests = 0 type, this is additive on top of
	 * whichever shape the type already has (idle/active/baseline_counter for
	 * serves_requests = 1, or the fpm_pool_status_s fields otherwise) -- a
	 * type reports through both if it has both kinds of state.
	 *
	 * NULL = no extra gauges (the common case). Called from the operator
	 * endpoint's own child, so -- same constraint as .status() -- everything
	 * read here must be shared memory or configuration, never another
	 * process's heap. Returns how many of the up to FPM_POOL_LIVE_GAUGES_MAX
	 * slots in out[] it filled. */
	int (*live_gauges)(struct fpm_worker_pool_s *wp, struct fpm_pool_live_gauge_s out[FPM_POOL_LIVE_GAUGES_MAX]);
};

/* Type with this name, or NULL. An empty name gives the default (fastcgi) type
 * — without this every existing fpm.conf would stop working. "fcgi" remains a
 * compatibility alias for "fastcgi". */
const struct fpm_pool_type_s *fpm_pool_type_get(const char *name);

/* Refuse an internal-only type that a pool SECTION named. fpm_pool_type_get()
 * finds such a type -- it has to, every pool resolves through it -- so the
 * "not configurable" half of .internal_only is checked here instead, where a
 * configuration is being read. 0 or -1. */
int fpm_pool_type_check_configurable(struct fpm_worker_pool_s *wp, const struct fpm_pool_type_s *type);

/* Effective variant resulting from pool.type + pool.executor, or NULL. */
const struct fpm_pool_type_s *fpm_pool_type_resolve(struct fpm_worker_pool_s *wp);

/* Check whether pool.executor is allowed and known. */
int fpm_pool_type_validate_executor(struct fpm_worker_pool_s *wp);

/* One line at startup naming this pool's tier, or nothing at all when the tier
 * is supported. Called once per pool from fpm_run(), in the master and before
 * the first fork -- see fpm_tier.h for why it is there and not per child. */
void fpm_pool_type_announce_tier(struct fpm_worker_pool_s *wp);

/* Names of known types for an error message. Buffer belongs to the caller. */
void fpm_pool_type_list(char *buf, size_t len);

/* What replaced a type name that used to exist, or NULL if the name was never
 * one of ours. A retired name earns its own error rather than being reported as
 * unknown: an unknown name is a typo, a retired one is a config file that used
 * to start. */
const char *fpm_pool_type_retired(const char *name);

/* Type of the given pool; never NULL after successful configuration validation. */
const struct fpm_pool_type_s *fpm_pool_type_of(struct fpm_worker_pool_s *wp);

/* Apply the type's listening-socket status flags in the master, before fork. */
int fpm_pool_type_prepare_listening_socket(struct fpm_worker_pool_s *wp);

/* Pool of the current child, or NULL outside a child. */
struct fpm_worker_pool_s *fpm_pool_type_current_pool(void);

/* Reject directives unsupported by this type. 0 or -1. */
int fpm_pool_type_check_directives(struct fpm_worker_pool_s *wp, const struct fpm_pool_type_s *type);

#endif
