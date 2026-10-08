/* fpm-ng: saturation numbers on the operator pages -- listen queue,
 * max_children_reached and slow requests (issue #644).
 *
 * Upstream FPM keeps all three in the pool's scoreboard, and the master writes
 * them. The listen queue is sampled in fpm_pctl_perform_idle_server_maintenance()
 * on every maintenance heartbeat (about one second). max_children_reached is
 * counted where the master finds pm.max_children reached. slow_rq is counted by
 * fpm_php_trace() when a request exceeds request_slowlog_timeout.
 *
 * This file only reads those fields, from a scoreboard copy the caller already
 * holds, and decides which of them the pool can honestly report. Three things
 * decide it, and none of them is the pool's name:
 *   - fpm_pool_type_s.reports_saturation, per type;
 *   - the platform: the listen queue is read with TCP_INFO (HAVE_LQ_TCP_INFO),
 *     so a build without it reports no listen queue;
 *   - the pool's own configuration: a listen queue exists only on a TCP
 *     listener, max_children_reached only where pm can reach pm.max_children,
 *     and slow requests only where request_slowlog_timeout is set.
 * A number that the pool cannot produce is omitted, never printed as zero.
 * docs/operator-endpoint.md describes the same rules for operators.
 */

#ifndef FPM_OPERATOR_SATURATION_H
#define FPM_OPERATOR_SATURATION_H 1

struct fpm_worker_pool_s;
struct fpm_scoreboard_s;
struct fpm_operator_buf_s;

struct fpm_operator_saturation_s {
	/* Listen queue. Present on a TCP listener in a build with TCP_INFO. */
	int has_listen_queue;
	unsigned long listen_queue; /* connections waiting for accept() now */
	unsigned long listen_queue_max; /* highest value seen since start */
	unsigned long listen_queue_length; /* backlog the kernel allows */

	/* pm.max_children reached. Present where pm is dynamic or ondemand: with
	 * pm = static the master never starts a child beyond the fixed count, so
	 * the counter would only ever read zero. */
	int has_max_children_reached;
	unsigned long max_children_reached;

	/* Slow requests. Present where request_slowlog_timeout is set: without it
	 * nothing traces a request, so nothing is counted. */
	int has_slow_requests;
	unsigned long slow_requests;
};

/* Fill *out for wp from `copy`, a scoreboard copy the caller holds. Everything
 * is cleared first, so a type without reports_saturation gets all has_* = 0. */
void fpm_operator_saturation_read(struct fpm_worker_pool_s *wp, const struct fpm_scoreboard_s *copy,
		struct fpm_operator_saturation_s *out);

/* Prometheus series for one pool, one line per value, labelled pool="<pool>". */
void fpm_operator_saturation_render_prometheus(struct fpm_operator_buf_s *b, const char *pool,
		const struct fpm_operator_saturation_s *sat);

/* Flat JSON keys, each with a leading comma, appended to an open object. */
void fpm_operator_saturation_render_json(struct fpm_operator_buf_s *b, const struct fpm_operator_saturation_s *sat);

#endif
