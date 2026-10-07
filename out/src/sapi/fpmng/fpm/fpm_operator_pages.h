/* fpm-ng: the two operator pages for one pool -- Prometheus text and JSON.
 *
 * Before issue #278 these were the two fixed paths of pool.type = status,
 * which aggregated every pool in the master. That pool is gone; the renderers
 * are not. They now produce the same two formats for the single pool that
 * configured operator.status_path / operator.metrics_path, and fpm_operator_endpoint.c
 * serves them on the operator listener. See docs/operator-endpoint.md and
 * docs/NOTES.md section 3u.
 */

#ifndef FPM_OPERATOR_PAGES_H
#define FPM_OPERATOR_PAGES_H 1

struct fpm_worker_pool_s;
struct fpm_operator_buf_s;

/* Render one pool into an operator HTTP response body.
 *
 * Called from the operator endpoint's own child, which is not one of the
 * pool's children, so these read only shared memory and configuration -- never
 * another process's heap. Everything they need has that property: the
 * scoreboard for a type that serves requests, and fpm_pool_type_s.status() for
 * a type that does not. */
void fpm_operator_page_render_prometheus(struct fpm_operator_buf_s *b, struct fpm_worker_pool_s *wp);
void fpm_operator_page_render_json(struct fpm_operator_buf_s *b, struct fpm_worker_pool_s *wp);

#endif
