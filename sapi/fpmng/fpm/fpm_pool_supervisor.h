/* fpm-ng: pool.type = supervisor — N long-lived processes running one PHP
 * script in-process (no exec), resurrected by the existing pm=static
 * machinery in fpm_children.c. See docs/NOTES.md for the design writeup
 * (in particular: why processes map onto pm=static + pm.max_children, and
 * why "give up" is a per-pool shared-memory state instead of a change to
 * fpm_children.c).
 *
 * issue #324: supervisor.max_memory (0 = disabled, default) recycles the
 * process — a plain, policy-exempt exit, same as pm.max_requests for a
 * classic worker — once its getrusage() RSS high-water mark reaches the
 * configured size. supervisor.stop_signal (default SIGTERM) is the signal
 * used both for that self-recycle and for an externally requested stop; see
 * docs/supervisor.md.
 */

#ifndef FPM_POOL_SUPERVISOR_H
#define FPM_POOL_SUPERVISOR_H 1

struct fpm_worker_pool_s;

/* Directives rejected for pool.type = supervisor. NULL-terminated, used as
 * .rejects in fpm_pool_types[]. */
extern const char *const fpm_pool_supervisor_rejects[];

/* fpm_pool_type_s.validate — supervisor-specific checks and defaults
 * (supervisor.script required, supervisor.restart valid, mapping
 * supervisor.processes to pm=static + pm.max_children). */
int fpm_pool_supervisor_validate(struct fpm_worker_pool_s *wp);

/* fpm_pool_type_s.init_main — allocate shared state (consecutive failure count,
 * backoff, terminal/gave_up) and register master-shutdown cleanup (for
 * supervisor.fatal). Called in the master before workers fork. */
int fpm_pool_supervisor_init_main(struct fpm_worker_pool_s *wp);

/* fpm_pool_type_s.child_main — child loop over script executions instead of
 * returning to the FastCGI accept loop. Does not return. Also registers
 * fpmng_supervisor_heartbeat() (issue #327) once for the process, the same way
 * fpm_http_direct.c registers fpm_connection_info() for its own pool type --
 * see the function's own comment in fpm_pool_supervisor.c. */
void fpm_pool_supervisor_child_main(struct fpm_worker_pool_s *wp);

struct fpm_pool_status_s;

/* fpm_pool_type_s.status — state for this pool as the operator pages read it.
 * Reads ONLY shared memory allocated by init_main (this is not the same process
 * as supervisor, so no local state is visible). */
void fpm_pool_supervisor_status(struct fpm_worker_pool_s *wp, struct fpm_pool_status_s *out);

#endif
