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

	/* Status flags are established on the master-side listening socket before
	 * children are forked. The open file description is shared by the master
	 * and its children, so a child must not change this after fork. */
	unsigned listening_socket_nonblocking:1;

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

	/* Directives unsupported by this type. NULL-terminated, may be NULL.
	 * A REJECTION list, not an allow-list — a new directive is allowed everywhere
	 * by default, so an omission does not break backward compatibility.
	 * A name ending in a dot works as a prefix: "pm." matches all pm.*. */
	const char *const *rejects;

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
};

/* Type with this name, or NULL. An empty name gives the default (fastcgi) type
 * — without this every existing fpm.conf would stop working. "fcgi" remains a
 * compatibility alias for "fastcgi". */
const struct fpm_pool_type_s *fpm_pool_type_get(const char *name);

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

#endif
