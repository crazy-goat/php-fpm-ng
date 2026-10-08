/* fpm-ng: the gateway's http.operator forwarding (issue #389): the <base>/<pool> namespace lookup and the request path that forwards into it.
 *
 * Split out of fpm_http.c (#747); a pure move. The shared definitions are in
 * fpm_http_internal.h. */
#include "fpm_config.h"

#include "fpm.h"
#include "fpm_http.h"

#ifdef HAVE_FPM_HTTP

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <grp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/keyvalq_struct.h>
#include <event2/util.h>

#include "php.h"
#include "fastcgi.h"
#include "zend_smart_str.h"

#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_sockets.h"
#include "fpm_cleanup.h"
#include "fpm_signals.h"
#include "fpm_env.h"
#include "fpm_shm.h"
#include "fpm_atomic.h"
#include "fpm_process_ctl.h"
#include "fpm_http_acl.h"
#include "fpm_http_accept_backoff.h"
#include "fpm_http_forwarded.h"
#include "fpm_acme_challenge.h"
#include "fpm_http_auth.h"
#include "fpm_http_access_log.h"
#include "fpm_http_direct_request.h"
#include "fpm_children_extra.h"
#include "fpm_pool_type.h"
#include "fpm_tls_http.h"
#include "fpm_tls_reload.h"
#include "fpm_http_static.h"
#include "fpm_child_error_log.h"
#include "fpm_error_log_follow.h"
#include "fpm_operator_http.h"
#include "fpm_operator_endpoint.h"
#include "zlog.h"

#include "fpm_http_internal.h"

/* ------------------------------------------------------------------ operator forwarding (issue #389) */

/* The gateway's OWN effective operator base for one format -- the root the
 * <base>/<pool name> forwarding hangs under. It is exactly the effective
 * operator.metrics_path / operator.status_path for this pool (docs/gateway.md):
 * on the gateway those default to /metrics and /status (#388), operator.X = on
 * derives /metrics/<pool name>, and an explicit "" (or operator.X = off) turns
 * the format off, which turns its forwarding off with it because there is no
 * base to forward under. Returns a string the caller must not free (config, or
 * the literal defaults), or the derived form in `scratch`. NULL = off.
 *
 * Only called for a proxy_only pool, where the defaults apply; the branch is
 * written out here rather than asking fpm_operator_endpoint.c because the
 * validation side needs it BEFORE the operator endpoint is configured, and it
 * is four lines of the same decision. */
const char *fpm_http_operator_base(struct fpm_worker_pool_s *wp, int metrics,
		char *scratch, size_t scratch_len)
{
	const char *pathname = metrics ? "operator.metrics_path" : "operator.status_path";
	const char *flagname = metrics ? "operator.metrics" : "operator.status";
	const char *configured = metrics ? wp->config->operator_metrics_path : wp->config->operator_status_path;
	int flag = metrics ? wp->config->operator_metrics : wp->config->operator_status;

	if (configured && *configured) {
		return configured;
	}
	if (fpm_conf_directive_was_set(wp->config, pathname)) {
		return NULL; /* explicit "" -- off; fpm_operator_endpoint.c logs the warning */
	}
	if (fpm_conf_directive_was_set(wp->config, flagname)) {
		if (!flag) {
			return NULL; /* explicit operator.X = off is honoured, not overwritten by the default */
		}
		snprintf(scratch, scratch_len, "%s/%s", metrics ? "/metrics" : "/status", wp->config->name);
		return scratch;
	}
	/* On the gateway both paths default to being set (#388). */
	return metrics ? "/metrics" : "/status";
}

/* Does `path` (query already cut) fall in the operator namespace this gateway
 * owns? Both bases are checked; the bare base and anything below it are in.
 * "/metricsx" is not -- the check is on the segment boundary, so a route named
 * /metricsx is not shadowed. A path outside the namespace is routed normally;
 * one inside it is answered by the map or by the gateway's own 404, never
 * forwarded anywhere else. */
static int fpm_http_operator_under_base(const struct fpm_http_gateway_s *gw, const char *path)
{
	const char *bases[2];
	unsigned i;

	bases[0] = gw->operator_metrics_base;
	bases[1] = gw->operator_status_base;
	for (i = 0; i < 2; i++) {
		size_t len;

		if (!bases[i]) {
			continue;
		}
		len = strlen(bases[i]);
		if (!strcmp(path, bases[i])) {
			return 1;
		}
		if (!strncmp(path, bases[i], len) && path[len] == '/') {
			return 1;
		}
	}
	return 0;
}

/* The exact-match map lookup. No prefix matching here on purpose: only a pool
 * that exposed itself is in the map, so /metrics/api is forwarded and
 * /metrics/anything-else is the gateway's own 404. */
static struct fpm_http_operator_entry_s *fpm_http_operator_lookup(struct fpm_http_gateway_s *gw,
		const char *path)
{
	unsigned i;

	for (i = 0; i < gw->noperator_entries; i++) {
		/* .path and .target are both non-NULL for every published row (see
		 * the INVARIANT on fpm_http_gateway_s.operator_entries); checked here
		 * too so a lookup can never hand a caller a targetless entry. */
		if (gw->operator_entries[i].path && gw->operator_entries[i].target && !strcmp(gw->operator_entries[i].path, path)) {
			return &gw->operator_entries[i];
		}
	}
	return NULL;
}

/* One request for the operator namespace of this gateway (issue #389).
 * Returns 1 when it answered (or took ownership of) the request, 0 when the
 * path is not in the namespace and normal handling should continue.
 *
 * Order, all of it load-bearing: this runs AFTER fpm_http_serve_ping() (a
 * ping is answered even if its path sits under a base) and BEFORE the static
 * lookup and routing, and the ACL is checked BEFORE the map lookup so a
 * stranger gets the same 403 for a pool that exists and one that does not.
 *
 * The map is exact-match on the RAW path (query cut, no percent-decoding) --
 * the same matcher ping.path and access.suppress_path[] use -- so "/%6detrics"
 * is not a way past it. A miss is a local 404 and never a forward: the
 * operator listener's own 404 lists every path it knows, which on loopback is
 * a convenience and on a public port would enumerate the pools. */
int fpm_http_operator_request(struct fpm_http_gateway_s *gw, struct evhttp_request *req,
		const char *peer_addr, const char *effective_addr, const struct fpm_http_forwarded_result_s *fwd,
		ev_uint16_t peer_port, struct fpm_http_client_s *client)
{
	const char *uri;
	const char *query;
	char path[512];
	size_t path_len, local_len, query_len;
	struct fpm_http_operator_entry_s *hit;
	fpm_http_conn *c;
	char *target_uri;

	if (!gw->operator_enabled) {
		return 0;
	}
	uri = evhttp_request_get_uri(req);
	if (!uri) {
		return 0;
	}
	/* Same libevent parse as the path and as the forwarded target (#534). */
	{
		const struct evhttp_uri *pu = evhttp_request_get_evhttp_uri(req);

		query = pu ? evhttp_uri_get_query(pu) : NULL;
	}
	path_len = fpm_http_raw_path(req, path, sizeof(path));
	if (!path_len) {
		return 0; /* empty, or too long to be one of this gateway's bases; route it */
	}

	if (!fpm_http_operator_under_base(gw, path)) {
		return 0;
	}

	/* The ACL is about the direct network peer, exactly like gw->acl, and is
	 * checked before the map so denied and nonexistent look the same. */
	if (gw->operator_acl && !fpm_http_acl_check(gw->operator_acl, peer_addr)) {
		fpm_http_log_response(gw, req, peer_addr, NULL, 403, 0, "operator");
		evhttp_send_error(req, 403, "Forbidden");
		fpm_http_count_local(gw); /* #390: answered here, not forwarded */
		return 1;
	}

	hit = fpm_http_operator_lookup(gw, path);
	if (!hit) {
		fpm_http_log_response(gw, req, effective_addr, NULL, HTTP_NOTFOUND, 0, "operator");
		evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
		fpm_http_count_local(gw); /* #390: a miss is the gateway's own 404 */
		return 1;
	}

	c = calloc(1, sizeof(*c));
	if (!c) {
		fpm_http_log_response(gw, req, effective_addr, NULL, FPM_HTTP_SERVICE_UNAVAIL, 0, "operator");
		evhttp_send_error(req, FPM_HTTP_SERVICE_UNAVAIL, "Service Unavailable");
		fpm_http_count_local(gw);
		return 1;
	}
	c->gw = gw;
	c->req = req;
	c->evcon = evhttp_request_get_connection(req);
	c->client = client; /* #390: the connection's gauge node */
	c->status = -1;
	c->queue_wait_ms = -1;
	c->fwd = *fwd;
	c->peer_port = peer_port;
	c->target = hit->target;
	c->log_target = "operator";
	if (peer_addr) {
		strlcpy(c->peer_addr, peer_addr, sizeof(c->peer_addr));
	}
	if (effective_addr) {
		strlcpy(c->remote_addr, effective_addr, sizeof(c->remote_addr));
	}

	/* The request line is rewritten to the operator listener's LOCAL path; the
	 * query string is preserved because "?json"/"?full" is the status page
	 * asked for a variant. Everything else about the request (method, body --
	 * monitored pages are GETs) is serialized by the #344 HTTP transport. */
	local_len = strlen(hit->local_uri);
	/* `query` is evhttp_uri_get_query(): WITHOUT the leading '?'. The '?' is
	 * written exactly once below -- writing it twice produced "/_m??json", and
	 * the operator listener's fpm_operator_http_has_flag() then never matched
	 * the variant. */
	query_len = query ? strlen(query) + 1 : 0;
	target_uri = malloc(local_len + query_len + 1);
	if (!target_uri) {
		fpm_http_log_response(gw, req, effective_addr, NULL, FPM_HTTP_SERVICE_UNAVAIL, 0, "operator");
		evhttp_send_error(req, FPM_HTTP_SERVICE_UNAVAIL, "Service Unavailable");
		fpm_http_conn_free(c);
		fpm_http_count_local(gw);
		return 1;
	}
	memcpy(target_uri, hit->local_uri, local_len);
	if (query_len) {
		target_uri[local_len] = '?';
		memcpy(target_uri + local_len + 1, query, query_len - 1);
	}
	target_uri[local_len + query_len] = '\0';
	c->upstream_uri_owned = target_uri;
	c->upstream_uri = target_uri;

	/* Issue #341's per-target counter describes traffic to a routed pool; an
	 * operator target is not in gw->targets and never appears in that page, so
	 * this only keeps the shared-struct field meaningful, it is not rendered. */
	fpm_http_counter_incr(c->target->requests_total);

	fpm_http_dispatch(gw, c, -1);
	return 1;
}

#endif /* HAVE_FPM_HTTP */
