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

struct fpm_worker_pool_s;
struct fpm_operator_reply_s;

/* State for a pool that does NOT handle requests (serves_requests = 0). For
 * fcgi/http pools (serves_requests = 1), the data shape is different
 * (idle/active/requests from the scoreboard) and this enum does not apply —
 * see pool.status in fpm_pool_status.c. */
enum fpm_pool_state_e {
	FPM_POOL_STATE_RUNNING = 0,	/* currently running a script */
	FPM_POOL_STATE_BACKOFF,		/* waiting during backoff before the next attempt (supervisor) */
	FPM_POOL_STATE_GAVE_UP,		/* gave up permanently, real failure (supervisor.restart_max) */
	FPM_POOL_STATE_FINISHED,	/* exited as planned, not a failure (restart=never/on-failure+success) */
	FPM_POOL_STATE_IDLE		/* doing nothing now, waiting for the next due time (cron between runs) */
};

/* Filled by fpm_pool_type_s.status() for types with serves_requests = 0.
 * Exactly the fields that pool.type = status actually shows — see
 * docs/NOTES.md section 3u. */
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
};

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

	/* Configuration requirements — read by fpm_conf.c, which does not know types. */
	unsigned requires_listen:1;		/* pool must have a listening address */
	unsigned requires_pm:1;			/* pool must have meaningful pm/pm.max_children */
	unsigned serves_requests:1;		/* counted in the request scoreboard */

	/* This type, in its OWN child, reads another pool's FOREIGN scoreboard
	 * (pool.type = status: idle/active/requests of other serves_requests=1 pools).
	 * Set only for "status". See fpm_children.c:
	 * fpm_child_resources_use() normally releases (munmaps) scoreboards for ALL
	 * pools except its own immediately after fork, as memory hygiene — safe because
	 * no type had read a foreign scoreboard until now. This flag disables the
	 * release ONLY for a child of THIS type (checked through
	 * fpm_pool_type_of(child->wp) in fpm_children.c); every other pool in the same
	 * configuration (including ordinary fcgi/http) still releases foreign
	 * scoreboards exactly as today, whether or not a status pool exists anywhere
	 * in the configuration. See docs/NOTES.md 3u. */
	unsigned reads_foreign_scoreboards:1;

	/* This type's own policy runs in the CHILD (supervisor backoff, restart_max,
	 * cron timeouts, "cannot open script"), so the messages an operator needs
	 * are emitted where upstream FPM assumes nothing worth logging happens and
	 * takes the error_log away — see fpm_child_log.h. Setting this gives the
	 * type's children a log channel back to the master; every zlog() in such a
	 * child then lands in error_log at its own level. Costs one socketpair per
	 * pool of this type, and nothing at all for any other pool.
	 *
	 * The same flag routes PHP's OWN diagnostics — errors, warnings, uncaught
	 * exceptions, error_log() from the script — into that channel instead of
	 * leaving them on a stdout nobody reads (issue #124,
	 * fpm_child_php_log.h): a child of such a type serves no request, so it has
	 * neither a response nor a front end's FastCGI stderr to put them in, which
	 * is the same premise as the log channel itself. */
	unsigned child_logs_via_master:1;

	/* A child of this type may publish HTTP-01 challenge answers, so it gets
	 * the fpmng_acme_challenge_* builtins (fpm_acme_challenge.h). Set for the
	 * script-running types that serve no request ("cron", where docs/NOTES.md
	 * section 3l puts the dedicated ACME process, and "supervisor"). Data
	 * rather than a name comparison in fpm_pool_script.c, and
	 * deliberately not set for request-serving types: a gateway must never
	 * execute the ACME client (issue #48, criterion 7). */
	unsigned publishes_acme_challenges:1;

	/* A child of this type keeps its FastCGI transport state and its signal
	 * handlers across requests instead of tearing them down and rebuilding
	 * them per request. It turns on two things in the child, both of which
	 * exist because upstream assumes a worker may be handed to an arbitrary
	 * front end between requests and we know it is not:
	 *
	 *   fcgi_set_optimized_transport()    patches 0004/0005: keep the
	 *     connection's buffers and use writev for large responses, instead of
	 *     the conservative per-request path main/fastcgi.c takes otherwise.
	 *   zend_signal_use_persistent_handlers()   patch 0006: install the Zend
	 *     signal handlers once instead of on every zend_signal_activate().
	 *
	 * Set it for a type whose children speak FastCGI over a connection the
	 * type itself owns for the child's lifetime -- "fastcgi-ng" and the
	 * workers behind "http", including their fiber and async variants. NOT for
	 * plain "fastcgi", whose connection comes from whatever front end dialled
	 * in, and not for "http-direct", which speaks HTTP itself and never
	 * touches main/fastcgi.c. See fpm.c, which reads this in the child. */
	unsigned reuses_request_runtime:1;

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
	 * Off for fastcgi and fastcgi-ng, where it changes what pm.status_path
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
	 * fpm_pool_status_render_json() produces for every type.
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
	 * It exists because the operator endpoint's directives live under "pm."
	 * (pm.status_path and friends, issue #273) while the types that most need
	 * that endpoint -- cron, supervisor -- reject the whole "pm." namespace,
	 * and for a good reason: their pm.* is generated programmatically, so a
	 * user-set one would be a second source of truth. The carve-out keeps that
	 * reason intact and names the handful of exceptions instead of weakening
	 * the prefix.
	 *
	 * Not by dropping the prefix and enumerating the ~20 real pm.* directives:
	 * that is the enumeration-versus-pattern mistake build/prepare.sh:75-79
	 * documents, and a pm.* added later would silently become legal on a cron
	 * pool. An exception must be added deliberately; a new directive must not
	 * become one by omission. */
	const char *const *reject_exceptions;

	/* Type-specific checks; NULL = none. Returns 0 or -1. */
	int (*validate)(struct fpm_worker_pool_s *wp);

	/* Master side, after validation and before child fork; NULL = none. */
	int (*init_main)(struct fpm_worker_pool_s *wp);

	/* What the child does instead of the accept loop; NULL = ordinary FastCGI
	 * loop. Does not return. */
	void (*child_main)(struct fpm_worker_pool_s *wp);

	/* How this type appears in pool.type = status when serves_requests = 0.
	 * NULL for serves_requests = 1 types (they have idle/active/requests from the
	 * scoreboard, read directly by fpm_pool_status.c) and for types without a
	 * meaningful state to show (for example, status itself). Called from ANOTHER
	 * process (the status pool), so it must read only shared memory/configuration,
	 * never process-local memory. */
	void (*status)(struct fpm_worker_pool_s *wp, struct fpm_pool_status_s *out);

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

/* Names of known types for an error message. Buffer belongs to the caller. */
void fpm_pool_type_list(char *buf, size_t len);

/* Type of the given pool; never NULL after successful configuration validation. */
const struct fpm_pool_type_s *fpm_pool_type_of(struct fpm_worker_pool_s *wp);

/* Apply the type's listening-socket status flags in the master, before fork. */
int fpm_pool_type_prepare_listening_socket(struct fpm_worker_pool_s *wp);

/* Pool of the current child, or NULL outside a child. */
struct fpm_worker_pool_s *fpm_pool_type_current_pool(void);

/* Reject directives unsupported by this type. 0 or -1. */
int fpm_pool_type_check_directives(struct fpm_worker_pool_s *wp, const struct fpm_pool_type_s *type);

/* Reject a type this BINARY cannot honour, as opposed to one this
 * CONFIGURATION misuses. 0 or -1. Always present; on the ordinary build it has
 * nothing to reject. See the definition for why it is keyed off the capability
 * bits rather than off type names. */
int fpm_pool_type_check_build_support(struct fpm_worker_pool_s *wp, const struct fpm_pool_type_s *type);

#endif
