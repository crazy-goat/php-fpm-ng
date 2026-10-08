/* fpm-ng: crash backoff for pm children that fail fast (issue #727).
 *
 * Upstream FPM respawns a child at once when it exits. A child that dies on
 * boot (a missing extension, a bad include) then makes the master fork in a
 * tight loop, and the logs fill with one line per fork. This module slows that
 * loop down for the types that set fpm_pool_type_s.respawn_backoff.
 *
 * Rules, all per pool:
 *   - A child is a fast failure when it exits within
 *     FPM_CRASH_BACKOFF_WINDOW_S seconds of its start and has served no request.
 *   - A child that served a request, or lived a whole window, ends the streak.
 *     A live child that reaches the window ends it too, while the child is
 *     still running (see the survive timer in fpm_crash_backoff.c).
 *   - The first fast failure respawns at once. Later ones wait a delay that
 *     doubles from 1 s up to a cap of about 60 s, with equal jitter.
 *   - When the streak reaches pm.max_consecutive_failures (0 = never), the
 *     pool gives up: one ALERT is logged and the operator pages say so. The
 *     retries go on at the capped rate.
 *
 * The state is shared memory, because the operator endpoint is a separate
 * process and reads it. It is allocated by fpm_crash_backoff_init() before the
 * first fork. The respawn timers stay in the master.
 */

#ifndef FPM_CRASH_BACKOFF_H
#define FPM_CRASH_BACKOFF_H 1

struct fpm_worker_pool_s;
struct fpm_child_s;
struct fpm_operator_buf_s;

/* A child that exits this soon after its start, with no request served, is a
 * fast failure. Fixed, not a directive: it only decides what counts as a boot
 * failure, and the cap is what an operator tunes against. */
#define FPM_CRASH_BACKOFF_WINDOW_S 10

/* What the operator endpoint reads about one pool. A copy, taken with
 * fpm_crash_backoff_read(). */
struct fpm_crash_backoff_snapshot_s {
	int has_state; /* 0 = this pool's type has no crash backoff */
	unsigned consecutive_failures; /* fast failures in a row, 0 after a healthy child */
	int gave_up; /* 1 once consecutive_failures reached pm.max_consecutive_failures */
	unsigned long respawn_delay_ms; /* the delay the next respawn waits for, 0 = none */
};

/* Called in the master for each pool, before the first fork. Allocates the
 * shared state when the pool's type has respawn_backoff, and does nothing
 * otherwise. Returns 0 on success, -1 when memory is not available. */
int fpm_crash_backoff_init(struct fpm_worker_pool_s *wp);

/* Called in the master from fpm_children_bury(), for a child that is going to
 * be respawned, before its scoreboard slot is freed (the slot holds the request
 * count this reads). */
void fpm_crash_backoff_child_exited(struct fpm_child_s *child);

/* Called in the master from fpm_children_make() before each fork. Returns 1 when
 * the fork may happen now. Returns 0 when the respawn must wait; then a one-shot
 * timer is armed and calls fpm_children_make() again when the wait is over. */
int fpm_crash_backoff_may_spawn(struct fpm_worker_pool_s *wp);

/* Called in the master after a fork, once child->started is set. While a streak
 * is running, it watches this child and ends the streak if it is still alive
 * after the window. */
void fpm_crash_backoff_child_spawned(struct fpm_child_s *child);

/* Copies the pool's crash backoff numbers into *out. Safe in any process that
 * has the shared state mapped, the operator endpoint included. */
void fpm_crash_backoff_read(struct fpm_worker_pool_s *wp, struct fpm_crash_backoff_snapshot_s *out);

/* Prometheus series for one pool, labelled pool="<pool>". Nothing is printed when
 * the pool has no crash backoff. */
void fpm_crash_backoff_render_prometheus(struct fpm_operator_buf_s *b, const char *pool,
		const struct fpm_crash_backoff_snapshot_s *snap);

/* Flat JSON keys, each with a leading comma, appended to an open object. Nothing
 * is printed when the pool has no crash backoff. */
void fpm_crash_backoff_render_json(struct fpm_operator_buf_s *b, const struct fpm_crash_backoff_snapshot_s *snap);

#endif
