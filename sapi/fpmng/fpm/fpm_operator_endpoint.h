/* fpm-ng: the per-pool operator endpoint (issue #274, decided in #273).
 *
 * A pool of a type that has nothing in front of it which could answer an
 * operator's scrape -- cron, supervisor, http, http-direct -- serves its own
 * stats and metrics from a small HTTP listener of its own:
 *
 *   pm.status_path     the pool's status page,   unset = off
 *   pm.metrics_path    Prometheus text,          unset = off
 *   pm.status_listen   where the above bind, default 127.0.0.1:8080
 *   pm.metrics_listen  the same, for the metrics path
 *
 * There is no separate on/off directive: the endpoint exists iff a path is set
 * (#273, point 4). With both paths unset nothing is bound at all.
 *
 * The metrics page is the same exposition format on every type, so that one
 * scraper can compare labelled series across pools. The status page is the
 * TYPE'S OWN where the type has one -- http-direct's carries per-connection
 * counters and a row per child (issue #64), and issue #275 moved that page here
 * unchanged rather than replacing it with the generic summary. Which types have
 * one is fpm_pool_type_s.operator_status, data like everything else here.
 *
 * Whether a type gets this at all is data, not a name comparison: it is
 * fpm_pool_type_s.operator_endpoint. On fastcgi the flag is off
 * and pm.status_path keeps its upstream meaning -- a path answered on the pool's
 * own FastCGI socket, with a web server in front of it -- because there the
 * front end is exactly what an operator already has (#273, point 2).
 *
 * WHOSE PROCESS ANSWERS. Not the master: a scrape does blocking I/O and
 * rendering, and the master is the one process in the tree whose death is
 * unrecoverable. Not the pool's own children either: an http-direct pool has
 * many, so a scrape would land on whichever one accepted, and a per-child
 * answer is not what an operator asked for. Instead every distinct listen
 * address gets ONE internal pool, created here, with a single child running the
 * shared operator HTTP server (fpm_operator_http.h). Pools that share a listen
 * address share that child; their paths are routes on it.
 *
 * That internal pool is an implementation detail and stays one -- it is not
 * pool.type = status, the user cannot configure it, cannot name it, and it does
 * not appear in any pool listing. What it is, is an ordinary entry in
 * fpm_worker_all_pools, so that it gets a listening socket, a supervised child
 * and a place in reload exactly like every other pool, instead of needing a
 * second supervision path invented for it.
 */

#ifndef FPM_OPERATOR_ENDPOINT_H
#define FPM_OPERATOR_ENDPOINT_H 1

struct fpm_worker_pool_s;
struct fpm_pool_type_s;

/* Directives rejected for the internal listener pool type. */
extern const char *const fpm_operator_endpoint_rejects[];

/* Register one configured pool's endpoint, creating or reusing the internal
 * listener pool for its address. Called once per pool from
 * fpm_conf_process_all_pools(), after the type is resolved. A pool whose type
 * has no operator endpoint, or which set no path, registers nothing.
 *
 * Returns -1 if two pools claim the same (address, port, path) -- the collision
 * rule from #273, point 7 -- or if a path is malformed. */
int fpm_operator_endpoint_configure(struct fpm_worker_pool_s *wp, const struct fpm_pool_type_s *type);

/* The four directives an operator endpoint owns, as they appear in a reject
 * list's .reject_exceptions. cron and supervisor reject the whole "pm."
 * namespace and carve these out (issue #283). */
extern const char *const fpm_operator_endpoint_directives[];

/* fpm_pool_type_s.validate / .child_main for the internal listener pool. */
int fpm_operator_endpoint_validate(struct fpm_worker_pool_s *wp);
void fpm_operator_endpoint_child_main(struct fpm_worker_pool_s *wp);

#endif
