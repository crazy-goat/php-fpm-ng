/* fpm-ng: saturation numbers on the operator pages (issue #644). The rules
 * for which numbers a pool reports are in fpm_operator_saturation.h. */

#include "fpm_config.h"

#include <string.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_type.h"
#include "fpm_scoreboard.h"
#include "fpm_operator_http.h"
#include "fpm_operator_saturation.h"

void fpm_operator_saturation_read(struct fpm_worker_pool_s *wp, const struct fpm_scoreboard_s *copy,
		struct fpm_operator_saturation_s *out) /* {{{ */
{
	const struct fpm_pool_type_s *type = fpm_pool_type_of(wp);

	memset(out, 0, sizeof(*out));
	if (!type->reports_saturation) {
		return;
	}

#ifdef HAVE_LQ_TCP_INFO
	/* Only a TCP listener has a TCP_INFO queue. The master samples the queue
	 * for AF_INET pools alone, so a unix listener's scoreboard keeps its zeros
	 * and is not reported. On a build without TCP_INFO (macOS development
	 * builds) nothing is sampled, and the series are left out for that reason.
	 * The BSD TCP_CONNECTION_INFO branch of fpm_socket_get_listening_queue()
	 * fills the field with a different counter, so it is not used here. */
	if (wp->listen_address_domain == FPM_AF_INET) {
		out->has_listen_queue = 1;
		out->listen_queue = copy->lq > 0 ? (unsigned long) copy->lq : 0;
		out->listen_queue_max = copy->lq_max > 0 ? (unsigned long) copy->lq_max : 0;
		out->listen_queue_length = copy->lq_len;
	}
#endif

	if (wp->config->pm != PM_STYLE_STATIC) {
		out->has_max_children_reached = 1;
		out->max_children_reached = copy->max_children_reached;
	}

	if (wp->config->request_slowlog_timeout > 0) {
		out->has_slow_requests = 1;
		out->slow_requests = copy->slow_rq;
	}
}
/* }}} */

void fpm_operator_saturation_render_prometheus(struct fpm_operator_buf_s *b, const char *pool,
		const struct fpm_operator_saturation_s *sat) /* {{{ */
{
	if (sat->has_listen_queue) {
		fpm_operator_buf_appendf(b,
				"fpmng_pool_listen_queue{pool=\"%s\"} %lu\n"
				"fpmng_pool_listen_queue_max{pool=\"%s\"} %lu\n"
				"fpmng_pool_listen_queue_length{pool=\"%s\"} %lu\n",
				pool, sat->listen_queue,
				pool, sat->listen_queue_max,
				pool, sat->listen_queue_length);
	}
	if (sat->has_max_children_reached) {
		fpm_operator_buf_appendf(b, "fpmng_pool_max_children_reached_total{pool=\"%s\"} %lu\n",
				pool, sat->max_children_reached);
	}
	if (sat->has_slow_requests) {
		fpm_operator_buf_appendf(b, "fpmng_pool_slow_requests_total{pool=\"%s\"} %lu\n",
				pool, sat->slow_requests);
	}
}
/* }}} */

void fpm_operator_saturation_render_json(struct fpm_operator_buf_s *b, const struct fpm_operator_saturation_s *sat) /* {{{ */
{
	if (sat->has_listen_queue) {
		fpm_operator_buf_appendf(b, ",\"listen_queue\":%lu,\"listen_queue_max\":%lu,\"listen_queue_length\":%lu",
				sat->listen_queue, sat->listen_queue_max, sat->listen_queue_length);
	}
	if (sat->has_max_children_reached) {
		fpm_operator_buf_appendf(b, ",\"max_children_reached\":%lu", sat->max_children_reached);
	}
	if (sat->has_slow_requests) {
		fpm_operator_buf_appendf(b, ",\"slow_requests\":%lu", sat->slow_requests);
	}
}
/* }}} */
