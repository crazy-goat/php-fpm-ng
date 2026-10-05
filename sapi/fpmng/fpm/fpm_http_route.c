/* fpm-ng: the gateway's http.route[] table, http.operator forwarding targets, and pool lifecycle (init, validate, metrics).
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

static int cleanup_registered = 0;

/* The directive takes precedence when actually set
 * (fpm_conf_directive_was_set — the value alone cannot distinguish "unset" from
 * "set to the default"); env remains a fallback for deployments that already
 * use it. http.allowed_clients is a new directive and deliberately has no
 * environment fallback. */
/* ---------------------------------------------------------- http.route[] */

/* Walks one http.route[] value, which is a comma-separated prefix list in the
 * convention listen.allowed_clients already uses for lists. *cursor starts at
 * the value and is advanced past each entry returned; whitespace around an
 * entry is trimmed. Returns 0 when the list is exhausted. */
static int fpm_http_route_next_prefix(const char **cursor, const char **out, size_t *out_len)
{
	const char *p = *cursor, *end;

	while (*p == ',' || *p == ' ' || *p == '\t') {
		p++;
	}
	if (!*p) {
		*cursor = p;
		return 0;
	}
	end = strchr(p, ',');
	if (!end) {
		end = p + strlen(p);
	}
	*cursor = end;
	while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
		end--;
	}
	*out = p;
	*out_len = (size_t) (end - p);
	return 1;
}

/* The HTTP/1.1 transport is intentionally cleartext, so it may only connect to
 * a target bound to a Unix socket or a numeric loopback IP literal. Refuse
 * hostnames as well as public addresses: resolving DNS here and again in each
 * gateway child would leave a rebinding window between validation and connect.
 * The transport resolves once per child, but a numeric-only contract makes the
 * result stable and auditable from the config. */
static int fpm_http_target_listen_is_loopback(char *address)
{
	char *copy, *host, *service, *end;
	struct addrinfo hints, *res = NULL, *ai;
	int rc, found = 0, safe = 1;

	if (fpm_sockets_domain_from_address(address) == FPM_AF_UNIX) {
		return 1;
	}
	copy = strdup(address);
	if (!copy) {
		return 0;
	}
	if (copy[0] == '[') {
		end = strchr(copy, ']');
		if (!end || end[1] != ':') {
			free(copy);
			return 0;
		}
		*end = '\0';
		host = copy + 1;
		service = end + 2;
	} else {
		service = strrchr(copy, ':');
		if (!service) {
			free(copy);
			return 0;
		}
		*service++ = '\0';
		host = copy;
	}
	if (!*host || !*service) {
		free(copy);
		return 0;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICHOST;
	rc = getaddrinfo(host, service, &hints, &res);
	if (rc != 0) {
		free(copy);
		return 0;
	}
	for (ai = res; ai; ai = ai->ai_next) {
		found = 1;
		if (ai->ai_family == AF_INET) {
			const struct sockaddr_in *sa = (const struct sockaddr_in *) ai->ai_addr;
			uint32_t host_order = ntohl(sa->sin_addr.s_addr);

			if ((host_order >> 24) != 127) {
				safe = 0;
			}
		} else if (ai->ai_family == AF_INET6) {
			const struct sockaddr_in6 *sa6 = (const struct sockaddr_in6 *) ai->ai_addr;

			/* Only the IPv6 loopback literal is admitted. In particular, reject
			 * IPv4-mapped addresses rather than depending on the listener's
			 * IPV6_V6ONLY setting to decide where they will route. */
			if (!IN6_IS_ADDR_LOOPBACK(&sa6->sin6_addr)) {
				safe = 0;
			}
		} else {
			safe = 0;
		}
	}
	freeaddrinfo(res);
	free(copy);
	return found && safe;
}

/* The pool one http.route[] key names, or NULL with the refusal already
 * logged. Every refusal here is a startup error: a route that names a pool
 * that does not exist, or one the gateway cannot speak to, would otherwise
 * become a 502 per request for a prefix the operator believes is configured.
 *
 * What a target speaks is asked OF THE TYPE (fpm_pool_type_s.serves_fastcgi /
 * .serves_http11), never of its name -- see fpm_pool_type.h. A pool.type =
 * http target has neither bit and lands in the last branch: a gateway in front
 * of a gateway is a nested proxy, and nothing about this issue makes it work. */
static struct fpm_worker_pool_s *fpm_http_route_target_pool(struct fpm_worker_pool_s *wp,
		const char *pool_name, enum fpm_http_transport_e *transport)
{
	struct fpm_worker_pool_s *w;
	const struct fpm_pool_type_s *type;

	for (w = fpm_worker_all_pools; w; w = w->next) {
		if (w->config && w->config->name && strcmp(w->config->name, pool_name) == 0) {
			break;
		}
	}
	if (!w) {
		zlog(ZLOG_ERROR, "[pool %s] http.route[%s]: no pool named '%s' is configured",
				wp->config->name, pool_name, pool_name);
		return NULL;
	}
	type = fpm_pool_type_resolve(w);
	if (!type) {
		zlog(ZLOG_ERROR, "[pool %s] http.route[%s]: pool '%s' has no usable pool.type/pool.executor combination",
				wp->config->name, pool_name, pool_name);
		return NULL;
	}
	if (type->serves_fastcgi) {
		*transport = FPM_HTTP_TARGET_FASTCGI;
		return w;
	}
	if (type->serves_http11) {
		/* Issue #344: the HTTP/1.1 client transport (fpm_http_client.c) speaks
		 * cleartext. Refuse both a target with its own TLS endpoint and any
		 * target whose listen address is not a Unix socket or numeric loopback
		 * IP, so routing cannot put client headers/payloads onto a public
		 * network in plaintext (issue #450). */
		if (w->config->http_tls_cert && *w->config->http_tls_cert) {
			zlog(ZLOG_ERROR, "[pool %s] http.route[%s]: pool '%s' terminates TLS on its own listener "
							 "(http.tls_cert); the gateway speaks cleartext to an http-direct target",
					wp->config->name, pool_name, pool_name);
			return NULL;
		}
		if (!fpm_http_target_listen_is_loopback(w->config->listen_address)) {
			zlog(ZLOG_ERROR, "[pool %s] http.route[%s]: pool '%s' listens on '%s'; the gateway speaks cleartext, "
							 "so an http-direct target must use a Unix socket or numeric loopback address",
					wp->config->name, pool_name, pool_name, w->config->listen_address);
			return NULL;
		}
		*transport = FPM_HTTP_TARGET_HTTP;
		return w;
	}
	zlog(ZLOG_ERROR, "[pool %s] http.route[%s]: pool '%s' is 'pool.type = %s', which the gateway cannot "
					 "use as a target; an http.route target must be a fastcgi or http-direct pool",
			wp->config->name, pool_name, pool_name, type->name);
	return NULL;
}

/* One prefix already claimed by this gateway's routes, for the duplicate
 * check. The pool it came from is kept so the refusal can name both sides. */
struct fpm_http_seen_prefix_s {
	const char *prefix;
	size_t len;
	const char *pool;
};

/* Everything about http.route[] that can be decided from the configuration
 * alone, checked in the master with every pool section already parsed (issue
 * #340). Called from fpm_http_validate_pool(), so `php-fpm-ng -t` refuses a
 * bad route table without anything having started.
 *
 * Refuses, each naming the pool and the offending directive: an unknown target
 * pool, a pool named twice, a target type the gateway cannot speak to, a
 * prefix without a leading '/', and a prefix claimed by two different entries.
 * An empty value is refused earlier, by the INI parser (fpm_conf.c), where the
 * line number is still known. */
static int fpm_http_validate_routes(struct fpm_worker_pool_s *wp)
{
	struct key_value_s *kv, *kv2;
	const char *name = wp->config->name;
	struct fpm_http_seen_prefix_s *seen;
	unsigned nseen = 0, total = 0;
	int rc = 0;

	if (!wp->config->http_routes) {
		return 0;
	}

	for (kv = wp->config->http_routes; kv; kv = kv->next) {
		const char *cursor = kv->value, *prefix;
		size_t prefix_len;

		while (fpm_http_route_next_prefix(&cursor, &prefix, &prefix_len)) {
			total++;
		}
	}
	if (!total) {
		zlog(ZLOG_ERROR, "[pool %s] http.route[]: no path prefix in any entry", name);
		return -1;
	}
	seen = calloc(total, sizeof(*seen));
	if (!seen) {
		zlog(ZLOG_ERROR, "[pool %s] http.route[]: out of memory", name);
		return -1;
	}

	for (kv = wp->config->http_routes; kv && rc == 0; kv = kv->next) {
		const char *cursor = kv->value, *prefix;
		size_t prefix_len;
		enum fpm_http_transport_e transport;

		for (kv2 = wp->config->http_routes; kv2 != kv; kv2 = kv2->next) {
			if (strcmp(kv2->key, kv->key) == 0) {
				zlog(ZLOG_ERROR, "[pool %s] http.route[%s]: pool '%s' is named twice; put every "
								 "prefix of one target in a single comma-separated value",
						name, kv->key, kv->key);
				rc = -1;
				break;
			}
		}
		if (rc != 0) {
			break;
		}
		if (!fpm_http_route_target_pool(wp, kv->key, &transport)) {
			rc = -1;
			break;
		}

		while (fpm_http_route_next_prefix(&cursor, &prefix, &prefix_len)) {
			unsigned i;

			if (prefix_len == 0 || prefix[0] != '/') {
				zlog(ZLOG_ERROR, "[pool %s] http.route[%s]: path prefix '%.*s' must begin with '/'",
						name, kv->key, (int) prefix_len, prefix);
				rc = -1;
				break;
			}
			for (i = 0; i < nseen; i++) {
				if (seen[i].len == prefix_len && memcmp(seen[i].prefix, prefix, prefix_len) == 0) {
					zlog(ZLOG_ERROR, "[pool %s] http.route[%s]: path prefix '%.*s' is already routed "
									 "to pool '%s'; one prefix selects one target",
							name, kv->key, (int) prefix_len, prefix, seen[i].pool);
					rc = -1;
					break;
				}
			}
			if (rc != 0) {
				break;
			}
			seen[nseen].prefix = prefix;
			seen[nseen].len = prefix_len;
			seen[nseen].pool = kv->key;
			nseen++;
		}
	}

	free(seen);
	return rc;
}

/* Sorts the table longest prefix first, so that the lookup is "the first row
 * that covers this path" and nothing at request time has to compare lengths.
 * An insertion sort over a table an operator wrote by hand. */
static void fpm_http_routes_sort(struct fpm_http_gateway_s *gw)
{
	unsigned i, j;

	for (i = 1; i < gw->nroutes; i++) {
		struct fpm_http_route_s row = gw->routes[i];

		for (j = i; j > 0 && gw->routes[j - 1].prefix_len < row.prefix_len; j--) {
			gw->routes[j] = gw->routes[j - 1];
		}
		gw->routes[j] = row;
	}
}

/* Fills one target in and points it at its row in the pool's counters segment.
 * own_capacity is the gateway's own connection budget, which differs from the
 * pool's child count for a multi-request executor (see fpm_http_init_pool_ex());
 * every other target is sized from its own pool's pm.max_children, the number
 * that pool's workers actually enforce.
 *
 * Issue #390: slot_index is the target's row in gw->counters -- [0, ntargets)
 * for the routed targets, the "operator" row for every operator listener.
 * fpm_http_routes_build() allocates the segment before the first target is
 * initialised, so nothing here allocates shared memory any more (issue #341's
 * three per-target fpm_shm_alloc() calls are gone); it either finds the
 * segment or fails. */
static int fpm_http_target_init(struct fpm_http_target_s *t, struct fpm_http_gateway_s *gw,
		unsigned slot_index, const char *pool, const char *listen_address,
		enum fpm_http_transport_e transport, unsigned capacity)
{
	atomic_t *slot;

	t->gw = gw;
	t->pool = strdup(pool);
	t->listen_address = strdup(listen_address);
	t->transport = transport;
	switch (transport) {
		case FPM_HTTP_TARGET_HTTP:
			t->ops = fpm_http_target_http_ops();
			break;
		case FPM_HTTP_TARGET_FASTCGI:
		default:
			t->ops = &fpm_http_target_fastcgi_ops;
			break;
	}
	t->max_upstreams = capacity ? capacity : 1;
	if (!t->pool || !t->listen_address || !gw->counters || slot_index >= gw->counters->nslots) {
		zlog(ZLOG_ERROR, "[pool %s] http: cannot allocate shared memory", gw->pool);
		return -1;
	}
	slot = fpm_http_counters_slot_cells(gw->counters, slot_index);
	t->slot_index = slot_index;
	t->upstreams_used = &slot[2];
	t->requests_total = &slot[0];
	t->rejected_total = &slot[1];
	return 0;
}

/* Builds the gateway's routing table, once, in the master, before the first
 * gateway forks (issue #340). fpm_http_validate_routes() has already refused
 * everything that could go wrong in the configuration, so the walk below only
 * has allocation left to fail on.
 *
 * "/" is an ordinary row. When no http.route[] entry claims it, the pool's own
 * listener is inserted as target 0 with prefix "/" -- a row in the same table,
 * not a fallback branch somewhere else -- which is what makes a gateway with no
 * routes exactly today's gateway and what leaves room for #345 to run one with
 * no row 0 at all. */
/* Issue #390: allocates the pool's one counters segment. The pool-wide,
 * monotonic part is sized from the route table: one slot per routed target,
 * plus one for "operator" and one for "-" (the requests answered locally). The
 * per-process gauge part is sized from http.gateways, because connections_open
 * and upstreams_held are "currently open" and must be summed over live
 * processes, not held once for the pool. Called before any target is
 * initialised, because every target's upstreams_used/requests_total/
 * rejected_total name a slot here; nslots = ntargets + 2 and nproc =
 * gw->nproc, which fpm_http_init_pool_ex() set just above. */
static int fpm_http_counters_alloc(struct fpm_http_gateway_s *gw, unsigned ntargets)
{
	unsigned nslots = ntargets + 2;
	unsigned nproc = gw->nproc ? gw->nproc : 1;
	size_t size = fpm_http_counters_size_of(nslots, nproc);

	gw->counters = fpm_shm_alloc(size);
	if (!gw->counters) {
		zlog(ZLOG_ERROR, "[pool %s] http: cannot allocate the gateway counters", gw->pool);
		return -1;
	}
	/* MAP_ANONYMOUS is zero-filled, but say so here rather than relying on a
	 * reader knowing mmap's contract: a respawned gateway must not find
	 * anything but zeroes in the segment it did not create. */
	memset(gw->counters, 0, size);
	gw->counters->nslots = nslots;
	gw->counters->nproc = nproc;
	return 0;
}

static int fpm_http_routes_build(struct fpm_worker_pool_s *wp, struct fpm_http_gateway_s *gw, unsigned own_capacity)
{
	struct key_value_s *kv;
	unsigned nentries = 0, nprefixes = 0, own, ti = 0, ri = 0;
	int root_claimed = 0;

	gw->has_routes = wp->config->http_routes != NULL;
	/* Issue #388: a gateway has no own pool to fall back to, so there is no
	 * row 0 and at least one route is mandatory. fpm_http_validate_pool()
	 * already refused the empty case with the operator-facing message; this
	 * is the defensive half, kept so a direct call cannot build a table with
	 * nothing in it. */
	if (gw->proxy_only && !wp->config->http_routes) {
		zlog(ZLOG_ERROR, "[pool %s] pool.type = gateway with no http.route[] serves nothing; "
						 "add at least one 'http.route[<pool>] = <prefix>'",
				wp->config->name);
		return -1;
	}
	own = gw->proxy_only ? 0 : 1;

	for (kv = wp->config->http_routes; kv; kv = kv->next) {
		const char *cursor = kv->value, *prefix;
		size_t prefix_len;

		nentries++;
		while (fpm_http_route_next_prefix(&cursor, &prefix, &prefix_len)) {
			nprefixes++;
			if (prefix_len == 1 && prefix[0] == '/') {
				root_claimed = 1;
				if (!gw->proxy_only) {
					own = 0; /* an entry claims "/" itself, so there is no row 0 to add */
				}
			}
		}
	}

	gw->targets = calloc(nentries + own, sizeof(*gw->targets));
	gw->routes = calloc(nprefixes + own, sizeof(*gw->routes));
	if (!gw->targets || !gw->routes) {
		zlog(ZLOG_ERROR, "[pool %s] http: cannot allocate the routing table", gw->pool);
		return -1;
	}

	/* Issue #390: the counters segment, sized from exactly this table, before
	 * any target points into it. */
	if (fpm_http_counters_alloc(gw, nentries + own) != 0) {
		return -1;
	}

	if (own) {
		if (fpm_http_target_init(&gw->targets[0], gw, 0, gw->pool, gw->listen_address,
					FPM_HTTP_TARGET_FASTCGI, own_capacity) != 0) {
			return -1;
		}
		gw->routes[0].prefix = strdup("/");
		if (!gw->routes[0].prefix) {
			zlog(ZLOG_ERROR, "[pool %s] http: cannot allocate the routing table", gw->pool);
			return -1;
		}
		gw->routes[0].prefix_len = 1;
		gw->routes[0].target = &gw->targets[0];
		ti = ri = 1;
	}

	for (kv = wp->config->http_routes; kv; kv = kv->next) {
		const char *cursor = kv->value, *prefix;
		size_t prefix_len;
		enum fpm_http_transport_e transport = FPM_HTTP_TARGET_FASTCGI;
		struct fpm_worker_pool_s *target = fpm_http_route_target_pool(wp, kv->key, &transport);
		unsigned capacity;

		if (!target) {
			return -1; /* unreachable: validate ran first */
		}
		capacity = target->config->pm_max_children > 0 ? (unsigned) target->config->pm_max_children : 1;
		if (fpm_http_target_init(&gw->targets[ti], gw, ti, target->config->name,
					target->config->listen_address, transport, capacity) != 0) {
			return -1;
		}
		while (fpm_http_route_next_prefix(&cursor, &prefix, &prefix_len)) {
			gw->routes[ri].prefix = strndup(prefix, prefix_len);
			if (!gw->routes[ri].prefix) {
				zlog(ZLOG_ERROR, "[pool %s] http: cannot allocate the routing table", gw->pool);
				return -1;
			}
			gw->routes[ri].prefix_len = prefix_len;
			gw->routes[ri].target = &gw->targets[ti];
			ri++;
		}
		ti++;
	}

	gw->ntargets = ti;
	gw->nroutes = ri;
	fpm_http_routes_sort(gw);

	/* Issue #388: a gateway whose routes claim no "/" is perfectly legal --
	 * every unmatched path is a local 404 -- but it is almost always a
	 * configuration the operator did not mean, so it is said once, here,
	 * where the whole table is in hand. A warning rather than a refusal:
	 * there are real uses (an API-only gateway) and the 404 is a correct
	 * answer, not a failure. */
	if (gw->proxy_only && !root_claimed) {
		zlog(ZLOG_NOTICE, "[pool %s] gateway: no http.route[] entry claims '/'; requests that "
						  "match no route are answered 404 by the gateway itself",
				wp->config->name);
	}

	return 0;
}

void fpm_http_routes_free(struct fpm_http_gateway_s *gw)
{
	unsigned i;

	for (i = 0; i < gw->nroutes; i++) {
		free(gw->routes[i].prefix);
	}
	for (i = 0; i < gw->ntargets; i++) {
		/* Issue #390: the shared memory is the segment below, not per target --
		 * these pointers name a row in it and must not be freed individually. */
		free(gw->targets[i].pool);
		free(gw->targets[i].listen_address);
	}
	/* Issue #390: the one counters segment, freed once. nslots/nproc are read
	 * before the munmap because the size is not stored anywhere else. */
	if (gw->counters) {
		fpm_shm_free(gw->counters, fpm_http_counters_size(gw->counters));
		gw->counters = NULL;
	}
	free(gw->routes);
	free(gw->targets);
	gw->routes = NULL;
	gw->targets = NULL;
	gw->nroutes = gw->ntargets = 0;
}

/* Issue #389: one transport target per distinct operator listener address.
 * Several pages share one listener, and the #344 HTTP/1.1 client a connection
 * is opened on belongs to the address, not to the page. */
static struct fpm_http_target_s *fpm_http_operator_target(struct fpm_http_gateway_s *gw, const char *address)
{
	unsigned i;
	struct fpm_http_target_s *t;

	for (i = 0; i < gw->noperator_targets; i++) {
		if (!strcmp(gw->operator_targets[i].listen_address, address)) {
			return &gw->operator_targets[i];
		}
	}
	t = &gw->operator_targets[gw->noperator_targets];
	/* Issue #390: every operator listener shares the one "operator" row, which
	 * sits right after the routed targets. The three pointers are not owned by
	 * this target, so a failure only has to release the strings. */
	if (fpm_http_target_init(t, gw, gw->ntargets, address, address,
				FPM_HTTP_TARGET_HTTP, FPM_HTTP_OPERATOR_UPSTREAMS) != 0) {
		free(t->pool);
		free(t->listen_address);
		memset(t, 0, sizeof(*t));
		return NULL;
	}
	return &gw->operator_targets[gw->noperator_targets++];
}

/* Issue #389: builds the exact-match map http.operator forwards from, once in
 * the master before the first gateway forks. Keys are this gateway's public
 * paths -- "<base>/<pool name>" for every pool that exposed itself, and the
 * bare base for the gateway's own page -- and values the operator listener
 * address and local path from fpm_operator_endpoint_route(). A pool that did
 * not expose the format (including a gateway whose DERIVED default page another
 * pool on the same address already owned, issue #388) has no route and gets no
 * entry: it is a local 404, never a forward.
 *
 * Called after every pool has been through fpm_operator_endpoint_configure(),
 * which the master's init_main pass guarantees. */
static int fpm_http_operator_build(struct fpm_worker_pool_s *wp, struct fpm_http_gateway_s *gw)
{
	char metrics_scratch[192], status_scratch[192];
	const char *bases[2];
	struct fpm_worker_pool_s *w;
	unsigned npools = 0, n = 0;
	int metrics;

	if (!gw->operator_enabled) {
		return 0;
	}

	{
		const char *b = fpm_http_operator_base(wp, 1, metrics_scratch, sizeof(metrics_scratch));

		gw->operator_metrics_base = b ? strdup(b) : NULL;
	}
	{
		const char *b = fpm_http_operator_base(wp, 0, status_scratch, sizeof(status_scratch));

		gw->operator_status_base = b ? strdup(b) : NULL;
	}
	bases[0] = gw->operator_metrics_base;
	bases[1] = gw->operator_status_base;

	if (!bases[0] && !bases[1]) {
		/* fpm_http_validate_pool() already refused http.operator = yes with
		 * both bases empty; this is the defensive half, kept so a direct call
		 * cannot build a gateway that forwards nothing. */
		zlog(ZLOG_ERROR, "[pool %s] http.operator = yes but both operator.metrics_path and "
						 "operator.status_path are empty; there is no base to forward under",
				wp->config->name);
		return -1;
	}

	for (w = fpm_worker_all_pools; w; w = w->next) {
		npools++;
	}
	/* At most one entry per format per pool; the gateway's own pages are among
	 * them (wp is in fpm_worker_all_pools). */
	gw->operator_entries = calloc(2 * npools + 2, sizeof(*gw->operator_entries));
	gw->operator_targets = calloc(2 * npools + 2, sizeof(*gw->operator_targets));
	if (!gw->operator_entries || !gw->operator_targets) {
		zlog(ZLOG_ERROR, "[pool %s] http.operator: cannot allocate the forwarding map", gw->pool);
		return -1;
	}

	for (w = fpm_worker_all_pools; w; w = w->next) {
		for (metrics = 1; metrics >= 0; metrics--) {
			const char *base = bases[metrics ? 0 : 1];
			const char *address, *local;
			struct fpm_http_target_s *t;
			char *key;

			if (!base) {
				continue;
			}
			if (!fpm_operator_endpoint_route(w, metrics, &address, &local)) {
				continue;
			}
			t = fpm_http_operator_target(gw, address);
			if (!t) {
				zlog(ZLOG_ERROR, "[pool %s] http.operator: cannot resolve the operator listener '%s'",
						gw->pool, address);
				return -1;
			}
			if (w == wp) {
				/* The gateway's own page sits at the bare base. */
				key = strdup(base);
			} else {
				size_t len = strlen(base) + 1 + strlen(w->config->name) + 1;

				key = malloc(len);
				if (key) {
					snprintf(key, len, "%s/%s", base, w->config->name);
				}
			}
			if (!key) {
				zlog(ZLOG_ERROR, "[pool %s] http.operator: cannot allocate the forwarding map", gw->pool);
				return -1;
			}
			/* Publish the row only once all three fields are set: t is the
			 * non-NULL target fpm_http_operator_target() returned above (the
			 * NULL case returns -1 before this point), key/local are its path
			 * and the operator listener's local path. Kept in step with the
			 * loop, not assigned once at the end: fpm_http_operator_free()
			 * frees exactly noperator_entries rows, so an allocation failure
			 * later in the loop must not strand the keys already built. This
			 * is the invariant fpm_http_gateway_s.operator_entries documents. */
			gw->operator_entries[n].path = key;
			gw->operator_entries[n].local_uri = local;
			gw->operator_entries[n].target = t;
			/* Issue #390: identity for the /metrics index. `w` is the pool
			 * this page belongs to, and `metrics` is which format the loop
			 * is on; `own` marks the gateway's own page, which the index
			 * skips because it lists what the gateway forwards. */
			gw->operator_entries[n].pool = w->config->name;
			gw->operator_entries[n].metrics = metrics ? 1 : 0;
			gw->operator_entries[n].own = (w == wp) ? 1 : 0;
			n++;
			gw->noperator_entries = n;
		}
	}

	for (n = 0; n < gw->noperator_entries; n++) {
		struct fpm_http_target_s *t = gw->operator_entries[n].target;

		/* Defensive: a published row always has a target (see the invariant
		 * above), but a skipped row beats a NULL dereference if that ever
		 * changes. */
		if (!t) {
			continue;
		}
		zlog(ZLOG_NOTICE, "[pool %s] http.operator: '%s' -> %s%s",
				gw->pool, gw->operator_entries[n].path, t->listen_address,
				gw->operator_entries[n].local_uri);
	}

	return 0;
}

/* Frees everything fpm_http_operator_build() allocated. Safe on a gw that never
 * built a map. */
void fpm_http_operator_free(struct fpm_http_gateway_s *gw)
{
	unsigned i;

	for (i = 0; i < gw->noperator_entries; i++) {
		free(gw->operator_entries[i].path);
	}
	free(gw->operator_entries);
	gw->operator_entries = NULL;
	gw->noperator_entries = 0;

	for (i = 0; i < gw->noperator_targets; i++) {
		/* Issue #390: no shared memory here -- the three counter pointers name
		 * the pool's "operator" row, freed with gw->counters by
		 * fpm_http_routes_free(). */
		free(gw->operator_targets[i].pool);
		free(gw->operator_targets[i].listen_address);
	}
	free(gw->operator_targets);
	gw->operator_targets = NULL;
	gw->noperator_targets = 0;

	free(gw->operator_metrics_base);
	free(gw->operator_status_base);
	gw->operator_metrics_base = gw->operator_status_base = NULL;
	fpm_http_acl_free(gw->operator_acl);
	gw->operator_acl = NULL;
	free(gw->operator_allowed_clients);
	gw->operator_allowed_clients = NULL;
}

static void fpm_http_gateway_settings(struct fpm_worker_pool_s *wp, struct fpm_http_gateway_s *gw, unsigned *nproc_wanted, int *reuseport_out) /* {{{ */
{
	const char *env;
	int idle_ms;
	const struct fpm_pool_type_s *type = fpm_pool_type_of(wp);

	/* Issue #388: every branch below that cares whether this is the pure
	 * proxy asks this flag, copied once here (in the master, before any fork,
	 * so every gateway process inherits it). */
	gw->proxy_only = type->proxy_only;

	if (fpm_conf_directive_was_set(wp->config, "http.gateways") && wp->config->http_gateways > 0) {
		*nproc_wanted = (unsigned) wp->config->http_gateways;
	} else if ((env = getenv("FPM_HTTP_GATEWAYS")) && atoi(env) > 0) {
		*nproc_wanted = (unsigned) atoi(env);
	} else {
		*nproc_wanted = wp->config->http_gateways > 0 ? (unsigned) wp->config->http_gateways : FPM_HTTP_GATEWAYS_DEFAULT;
	}

	if (fpm_conf_directive_was_set(wp->config, "http.reuseport")) {
		*reuseport_out = wp->config->http_reuseport;
	} else {
		env = getenv("FPM_HTTP_REUSEPORT");
		*reuseport_out = env && atoi(env) > 0;
	}
	gw->reuseport = *reuseport_out;

	/* No environment fallback, on purpose -- see
	 * fpm_http_upstream_write_must_fail(). */
	gw->fault_write_at = wp->config->http_fault_upstream_write > 0
								 ? wp->config->http_fault_upstream_write
								 : 0;

	if (fpm_conf_directive_was_set(wp->config, "http.static")) {
		gw->static_files = wp->config->http_static;
	} else if ((env = getenv("FPM_HTTP_STATIC"))) {
		gw->static_files = atoi(env) > 0;
	} else {
		gw->static_files = wp->config->http_static;
	}

	if (fpm_conf_directive_was_set(wp->config, "http.idle_timeout")) {
		idle_ms = wp->config->http_idle_timeout;
	} else if ((env = getenv("FPM_HTTP_IDLE_MS"))) {
		idle_ms = atoi(env);
	} else {
		idle_ms = wp->config->http_idle_timeout;
	}
	gw->idle_ms = idle_ms;
	gw->idle_timeout.tv_sec = idle_ms / 1000;
	gw->idle_timeout.tv_usec = (idle_ms % 1000) * 1000;

	gw->read_timeout_ms = wp->config->http_read_timeout;
	gw->read_timeout.tv_sec = wp->config->http_read_timeout / 1000;
	gw->read_timeout.tv_usec = (wp->config->http_read_timeout % 1000) * 1000;
	gw->keepalive_timeout_ms = wp->config->http_keepalive_timeout;
	gw->keepalive_timeout.tv_sec = wp->config->http_keepalive_timeout / 1000;
	gw->keepalive_timeout.tv_usec = (wp->config->http_keepalive_timeout % 1000) * 1000;
	gw->write_timeout_ms = wp->config->http_write_timeout;
	gw->write_timeout.tv_sec = wp->config->http_write_timeout / 1000;
	gw->write_timeout.tv_usec = (wp->config->http_write_timeout % 1000) * 1000;
	gw->response_buffer = wp->config->http_response_buffer;
	gw->max_body = wp->config->http_max_body;

	gw->wait_policy = wp->config->http_pool_full_policy;
	gw->wait_queue_max = wp->config->http_pool_full_queue_max;
	gw->wait_ms = wp->config->http_pool_full_wait_ms;
	gw->wait_bound.tv_sec = gw->wait_ms / 1000;
	gw->wait_bound.tv_usec = (gw->wait_ms % 1000) * 1000;

	/* Issue #388: on the gateway, `listen` IS the public port, so http.listen
	 * (and its environment fallback) has nothing to override and
	 * fpm_http_validate_pool() refuses it. Ignoring it here too keeps a
	 * direct call from reaching fpm_http_listen() with a stale override. */
	if (!gw->proxy_only) {
		if (fpm_conf_directive_was_set(wp->config, "http.listen") && wp->config->http_listen && *wp->config->http_listen) {
			gw->http_listen_override = strdup(wp->config->http_listen);
		} else if ((env = getenv("FPM_HTTP_LISTEN")) && *env) {
			gw->http_listen_override = strdup(env);
		}
	}
	if (wp->config->http_plain_listen && *wp->config->http_plain_listen) {
		gw->plain_listen_address = strdup(wp->config->http_plain_listen);
	}

	if (wp->config->http_allowed_clients && *wp->config->http_allowed_clients) {
		gw->allowed_clients = strdup(wp->config->http_allowed_clients);
	}

	/* Issue #389: http.operator/what it does with the public port. The map
	 * itself is built later, by fpm_http_operator_build(), once every pool's
	 * operator routes exist. */
	gw->operator_enabled = wp->config->http_operator;
	if (wp->config->http_operator_allowed_clients && *wp->config->http_operator_allowed_clients) {
		gw->operator_allowed_clients = strdup(wp->config->http_operator_allowed_clients);
	}

	if (wp->config->http_trusted_proxies && *wp->config->http_trusted_proxies) {
		gw->trusted_proxies = strdup(wp->config->http_trusted_proxies);
	}

	if (wp->config->http_access_log && *wp->config->http_access_log) {
		gw->access_log_path = strdup(wp->config->http_access_log);
	}

	/* ping.path/ping.response -- issue #382. fpm_conf.c has already validated
	 * ping_path and, when it is set, filled in ping_response with "pong" if
	 * the pool did not set one, so nothing is re-validated here. */
	if (wp->config->ping_path && *wp->config->ping_path) {
		gw->ping_path = strdup(wp->config->ping_path);
		gw->ping_response = strdup(wp->config->ping_response ? wp->config->ping_response : "pong");
	}

	/* access.suppress_path[], copied the same way http-direct's access log
	 * does (fpm_http_direct_access_log.c) -- see fpm_http_log_suppressed(). */
	{
		struct key_value_s *kv;
		unsigned n = 0;

		for (kv = wp->config->access_suppress_paths; kv; kv = kv->next) {
			n++;
		}
		if (n) {
			gw->suppress_paths = calloc(n, sizeof(*gw->suppress_paths));
			if (gw->suppress_paths) {
				for (kv = wp->config->access_suppress_paths; kv; kv = kv->next) {
					gw->suppress_paths[gw->suppress_paths_count] = strdup(kv->value);
					if (!gw->suppress_paths[gw->suppress_paths_count]) {
						break;
					}
					gw->suppress_paths_count++;
				}
			}
		}
	}

	/* Default is "/index.php" (see the struct field's init in fpm_conf.c), so an
	 * unset directive already arrives here non-empty; strdup("") when the pool
	 * explicitly blanked it out to disable the fallback. */
	gw->front_controller = strdup(wp->config->http_front_controller ? wp->config->http_front_controller : "");

	/* The gateway drops to the same identity as the pool's own workers once
	 * it no longer needs root, see fpm_http_gateway_drop_privileges(). */
	gw->drop_uid = (uid_t) wp->set_uid;
	gw->drop_gid = (gid_t) wp->set_gid;
	/* wp->set_user is only populated when 'user' was a numeric id (see
	 * fpm_unix_conf_wp() in fpm_unix.c); otherwise fall back to the name as
	 * configured, exactly like fpm_unix_init_child() does for workers. */
	if (wp->set_user) {
		gw->drop_user = strdup(wp->set_user);
	} else if (wp->config->user && *wp->config->user) {
		gw->drop_user = strdup(wp->config->user);
	}

#ifdef HAVE_FPM_HTTP_TLS
	/* fpm_http_validate_pool() already refused a bad/mismatched cert+key at
	 * config-validation time; this is the real load, in the master, BEFORE
	 * fpm_http_gateway_spawn() forks the first child -- see fpm_tls_http.h. */
	gw->tls_wait_for_cert = wp->config->http_tls_wait_for_cert;
	/* Skipped entirely in NO_CERT rather than attempted and allowed to fail:
	 * fpm_tls_http_load() logs "cannot re-read TLS certificate/key at
	 * startup" at ERROR level, and under http.tls_wait_for_cert a
	 * not-yet-issued certificate is the configured state, not a fault. An
	 * ERROR on every first boot would train an operator to ignore the one
	 * line that does mean something. The same access() pair that
	 * fpm_http_validate_pool() used to decide whether to skip the validate
	 * decides here, so the two cannot disagree. */
	if (wp->config->http_tls_cert && *wp->config->http_tls_cert &&
			!(gw->tls_wait_for_cert &&
					(access(wp->config->http_tls_cert, R_OK) != 0 ||
							!wp->config->http_tls_key || access(wp->config->http_tls_key, R_OK) != 0))) {
		gw->tls = fpm_tls_http_load(gw->pool, wp->config->http_tls_cert,
				wp->config->http_tls_key, wp->config->http_tls_min_version,
				wp->config->http_tls_sni_cert,
				wp->config->http_tls_verify_client, wp->config->http_tls_client_ca);
	}
	/* The certificate was there all along, so there is nothing to wait for:
	 * drop the opt-in and let every gate below behave exactly as it does for
	 * a pool that never set it. Without this the pool stays flagged as
	 * waiting, and if fpm_tls_reload_master_init() then fails -- a chain
	 * over FPM_TLS_RELOAD_MAX_CERT, or no shared memory -- the master
	 * binds :443 without listening while no child ever registers a hook to
	 * open it, and a pool holding a perfectly good certificate refuses every
	 * connection for the life of the master. */
	if (gw->tls) {
		gw->tls_wait_for_cert = 0;
	}
	if (gw->tls || gw->tls_wait_for_cert) {
		/* http.tls_reload_check: unset -> a sensible non-zero default (task
		 * 040 exists precisely so a renewed certificate needs no operator
		 * action beyond the write); explicitly 0 -> off. Same
		 * was-it-set-at-all pattern as http.gateways above. */
		int interval = fpm_conf_directive_was_set(wp->config, "http.tls_reload_check")
							   ? wp->config->http_tls_reload_check
							   : FPM_TLS_RELOAD_CHECK_DEFAULT;

		if (interval < 0) {
			interval = 0;
		}
		/* gw->tls is NULL here exactly when http.tls_wait_for_cert put this
		 * pool in NO_CERT (issue #172); fpm_http_validate_pool() has already
		 * refused the combination with http.tls_reload_check = 0, so the
		 * timer below is guaranteed to be armed and the state is guaranteed
		 * to be escapable. */
		gw->reload = fpm_tls_reload_master_init(gw->pool, wp->config->http_tls_cert,
				wp->config->http_tls_key, wp->config->http_tls_min_version, gw->tls, interval);
		if (gw->tls_wait_for_cert && !gw->tls) {
			if (!gw->reload) {
				/* Without the reload machinery there is no mechanism that can
				 * ever open the TLS listener, so the pool would sit in NO_CERT
				 * for the life of the master with :443 refusing every
				 * connection. Dropping the opt-in turns that into the ordinary
				 * fail-closed startup error the operator can act on. */
				zlog(ZLOG_ERROR, "[pool %s] http.tls_wait_for_cert: the certificate-watch machinery could not be set up, so nothing would ever open the TLS listener", gw->pool);
				gw->tls_wait_for_cert = 0;
			} else {
				zlog(ZLOG_NOTICE, "[pool %s] http: NO_CERT -- no certificate at '%s' yet; the TLS listener stays closed and http.plain_listen answers ACME HTTP-01 challenges only, re-checking every %d second(s)",
						gw->pool, wp->config->http_tls_cert, interval);
			}
		}
	}
#endif
}
/* }}} */

/* Called once per gateway pool by the master, before any child forks.
 * capacity_override is needed by multi-request executors: a classic worker
 * holds one connection, while a Fiber holds many. 0 preserves the child-count
 * limit. On the gateway (fpm_pool_type_s.proxy_only) there are no bundled
 * workers at all: the target counts come from each target's own
 * pm.max_children, and the process count from http.gateways. */
static int fpm_http_init_pool_ex(struct fpm_worker_pool_s *wp, unsigned capacity_override) /* {{{ */
{
	char cwd[MAXPATHLEN];

	if (!getcwd(cwd, sizeof(cwd))) {
		strcpy(cwd, "/");
	}

	{
		struct fpm_http_gateway_s *gw;
		unsigned workers = wp->config->pm_max_children > 0 ? (unsigned) wp->config->pm_max_children : 1;
		unsigned capacity = capacity_override ? capacity_override : workers;
		const char *capacity_env = capacity_override ? getenv("FPM_HTTP_MAX_UPSTREAMS") : NULL;
		unsigned nproc_wanted;
		int reuseport;
		unsigned i;

		if (capacity_env && atoi(capacity_env) > 0) {
			capacity = (unsigned) atoi(capacity_env);
		}

		gw = calloc(1, sizeof(*gw));
		gw->listen_fd = -1;
		gw->plain_listen_fd = -1;
		gw->pool = strdup(wp->config->name);
		gw->listen_address = strdup(wp->config->listen_address);
		gw->docroot = strdup(wp->config->chdir && *wp->config->chdir ? wp->config->chdir : cwd);
		gw->backlog = wp->config->listen_backlog;
		fpm_http_gateway_settings(wp, gw, &nproc_wanted, &reuseport);
		/* Needs gw->docroot and gw->front_controller, both set above; runs once
		 * here in the master so every forked gateway process inherits the
		 * verdict instead of each evaluating it on its own first request. */
		fpm_http_front_controller_validate(gw);

#ifdef HAVE_FPM_HTTP_TLS
		/* fpm_tls_http_load() already logged what went wrong; http.tls_cert
		 * was set, so falling back to plain HTTP would be a silent surprise.
		 *
		 * http.tls_wait_for_cert (issue #172) is the one way past this, and
		 * it is not a downgrade: the pool does not fall back to plain HTTP,
		 * it declines to serve :443 at all until the certificate exists. The
		 * check above is on gw->tls_wait_for_cert rather than on the config
		 * field because fpm_http_gateway_settings() clears it when the
		 * certificate-watch machinery failed to start, and then this gate
		 * must fire exactly as it always did. */
		if (wp->config->http_tls_cert && *wp->config->http_tls_cert && !gw->tls && !gw->tls_wait_for_cert) {
			fpm_http_operator_free(gw); /* issue #389 */
			free(gw->allowed_clients);
			free(gw->trusted_proxies);
			free(gw->access_log_path);
			free(gw->http_listen_override);
			free(gw->plain_listen_address);
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return -1;
		}
#endif

		/* a UNIX socket pool has no port to bump, so it needs an explicit HTTP
		 * address — fpm_http_validate_pool() already refused to start without
		 * one; this is just a defensive fallback, unreachable in practice.
		 * Issue #388: not on the gateway, where `listen` IS the public
		 * address and there is nothing to bump — it may be a unix socket. */
		if (!gw->proxy_only && wp->listen_address_domain != FPM_AF_INET && !gw->http_listen_override) {
			fpm_http_operator_free(gw); /* issue #389 */
			free(gw->allowed_clients);
			free(gw->trusted_proxies);
			free(gw->front_controller);
			free(gw->access_log_path);
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return 0;
		}

		if (gw->allowed_clients && fpm_http_acl_parse(gw->pool, "http.allowed_clients", gw->allowed_clients, &gw->acl) != 0) {
			fpm_http_operator_free(gw); /* issue #389 */
			free(gw->allowed_clients);
			free(gw->trusted_proxies);
			free(gw->front_controller);
			free(gw->access_log_path);
			free(gw->http_listen_override);
			free(gw->plain_listen_address);
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return -1;
		}

		if (gw->trusted_proxies && fpm_http_acl_parse(gw->pool, "http.trusted_proxies", gw->trusted_proxies, &gw->trusted_proxies_acl) != 0) {
			fpm_http_operator_free(gw); /* issue #389 */
			fpm_http_acl_free(gw->acl);
			free(gw->allowed_clients);
			free(gw->trusted_proxies);
			free(gw->front_controller);
			free(gw->access_log_path);
			free(gw->http_listen_override);
			free(gw->plain_listen_address);
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return -1;
		}

		/* Issue #389: the ACL for the forwarded operator pages, separate from
		 * http.allowed_clients on purpose -- one may be open while the other
		 * is not. Parsed here, in the master, like the other two ACLs. */
		if (gw->operator_allowed_clients && fpm_http_acl_parse(gw->pool, "http.operator_allowed_clients",
													gw->operator_allowed_clients, &gw->operator_acl) != 0) {
			fpm_http_acl_free(gw->acl);
			free(gw->allowed_clients);
			fpm_http_acl_free(gw->trusted_proxies_acl);
			free(gw->trusted_proxies);
			free(gw->front_controller);
			free(gw->access_log_path);
			free(gw->http_listen_override);
			free(gw->plain_listen_address);
			fpm_http_operator_free(gw); /* issue #389: also operator_allowed_clients */
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return -1;
		}

		if (gw->proxy_only) {
			/* Issue #388: on the gateway `listen` is the public port. For a
			 * TCP address the master binds it here through fpm_http_listen(),
			 * NOT by reusing the socket upstream's fpm_sockets_init_main()
			 * would have created: fpm_http_validate_pool() zeroed the domain
			 * so that socket does not exist. fpm_http_listen() sets
			 * SO_REUSEPORT before bind(), so a reuseport group works, and the
			 * socket is not in upstream's sockets_list, so an exec-reload
			 * never exports a closed fd. do_listen = 0 in NO_CERT keeps the
			 * bind-but-do-not-listen state. */
			enum fpm_address_domain listen_domain = fpm_sockets_domain_from_address(gw->listen_address);

			if (listen_domain == FPM_AF_UNIX) {
				/* fpm_http_listen() parses host:port only, so a unix public
				 * listener keeps using the master's socket (dup()ed so this
				 * family owns its own descriptor). reuseport is meaningless
				 * on a unix socket; force it off. */
				gw->listen_fd = dup(wp->listening_socket);
				reuseport = 0;
				gw->reuseport = 0;
			} else {
				gw->listen_fd = fpm_http_listen(gw->pool, gw->listen_address, gw->listen_address,
						gw->backlog, reuseport, !gw->tls_wait_for_cert);
				/* No master-owned socket for this pool: make the unused slot
				 * explicit so nothing -- the child's listener-socket close
				 * loop in particular -- treats fd 0 as this pool's listener. */
				wp->listening_socket = -1;
			}
		} else {
			gw->listen_fd = fpm_http_listen(gw->pool, gw->listen_address, gw->http_listen_override, gw->backlog, reuseport, !gw->tls_wait_for_cert);
		}
		if (gw->listen_fd < 0) {
			fpm_http_operator_free(gw); /* issue #389 */
			fpm_http_acl_free(gw->acl);
			free(gw->allowed_clients);
			fpm_http_acl_free(gw->trusted_proxies_acl);
			free(gw->trusted_proxies);
			free(gw->front_controller);
			free(gw->access_log_path);
			free(gw->http_listen_override);
			free(gw->plain_listen_address);
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return 0;
		}
		if (gw->plain_listen_address) {
			gw->plain_listen_fd = fpm_http_listen(gw->pool, gw->listen_address, gw->plain_listen_address, gw->backlog, reuseport, 1);
			if (gw->plain_listen_fd < 0) {
				close(gw->listen_fd);
				fpm_http_operator_free(gw); /* issue #389 */
				fpm_http_acl_free(gw->acl);
				free(gw->allowed_clients);
				fpm_http_acl_free(gw->trusted_proxies_acl);
				free(gw->trusted_proxies);
				free(gw->front_controller);
				free(gw->access_log_path);
				free(gw->http_listen_override);
				free(gw->plain_listen_address);
				free(gw->pool);
				free(gw->listen_address);
				free(gw->docroot);
				free(gw);
				return 0;
			}
		}
		/* A classic worker handles one connection at a time; a multi-request
		 * executor supplies its own capacity independently of the child count.
		 * Issue #388: the gateway has no pm.max_children and no children of
		 * its own to cap http.gateways against, so its process count is
		 * http.gateways alone -- the old MIN() deliberately does not apply. */
		gw->nproc = gw->proxy_only ? nproc_wanted : MIN(nproc_wanted, workers);
		/* The routing table, built once here in the master so that every
		 * gateway process inherits the same targets and shares one budget
		 * counter per target (issue #340). Issue #388: on the gateway it is
		 * exactly http.route[] -- no implicit own-pool row, and
		 * fpm_http_validate_pool() has refused a table with no entries. */
		if (fpm_http_routes_build(wp, gw, capacity) != 0) {
			fpm_http_routes_free(gw);
			fpm_http_operator_free(gw); /* issue #389 */
			close(gw->listen_fd);
			fpm_http_acl_free(gw->acl);
			free(gw->allowed_clients);
			fpm_http_acl_free(gw->trusted_proxies_acl);
			free(gw->trusted_proxies);
			free(gw->front_controller);
			free(gw->access_log_path);
			free(gw->http_listen_override);
			free(gw->plain_listen_address);
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return -1;
		}
		/* Issue #389: the http.operator forwarding map, built once here in the
		 * master from every pool's operator routes and inherited by every
		 * gateway process through fork(). A reload rebuilds it like every
		 * other http.* setting. */
		if (fpm_http_operator_build(wp, gw) != 0) {
			fpm_http_operator_free(gw);
			fpm_http_routes_free(gw);
			close(gw->listen_fd);
			fpm_http_acl_free(gw->acl);
			free(gw->allowed_clients);
			fpm_http_acl_free(gw->trusted_proxies_acl);
			free(gw->trusted_proxies);
			free(gw->front_controller);
			free(gw->access_log_path);
			free(gw->http_listen_override);
			free(gw->plain_listen_address);
			free(gw->pool);
			free(gw->listen_address);
			free(gw->docroot);
			free(gw);
			return -1;
		}
		gw->pids = calloc(gw->nproc, sizeof(pid_t));
		/* array of pointers — sizeof(void *) is intentional */
		gw->slots = calloc(gw->nproc, sizeof(void *));
		gw->next = gateways;
		gateways = gw;
		if (gw->proxy_only) {
			/* Issue #388: there is no "pool" behind a gateway, so the old
			 * line's persistent-connection count (sized from pm.max_children)
			 * has nothing to describe; the public address is what an operator
			 * needs here. Each target's own count is on its http target line. */
			zlog(ZLOG_NOTICE, "[pool %s] HTTP listener: %u gateway(s)%s%s on %s",
					wp->config->name, gw->nproc, reuseport ? " with SO_REUSEPORT" : "",
					gw->acl ? ", access-restricted" : "", gw->listen_address);
		} else {
			zlog(ZLOG_NOTICE, "[pool %s] HTTP listener: %u gateway(s)%s%s, %u persistent connection(s) to the pool",
					wp->config->name, gw->nproc, reuseport ? " with SO_REUSEPORT" : "",
					gw->acl ? ", access-restricted" : "", capacity);
		}
		if (wp->config->http_routes) {
			/* Printed in lookup order, longest prefix first, because that is
			 * the order a request is matched in and the only way to read the
			 * table is to read it the way the gateway does. */
			for (i = 0; i < gw->nroutes; i++) {
				zlog(ZLOG_NOTICE, "[pool %s] http.route: '%s' -> pool %s (%u persistent connection(s))",
						wp->config->name, gw->routes[i].prefix, gw->routes[i].target->pool,
						gw->routes[i].target->max_upstreams);
			}
		}
		/* Issue #341: one line per TARGET, pool/address/capacity, as opposed to
		 * the http.route[] lines above which are one line per PREFIX (several
		 * of which may name the same target -- see fpm_http_target_s's own
		 * comment on why the budget is per pool and not per prefix). Printed
		 * unconditionally, including the unrouted case where ntargets == 1 and
		 * this is the pool's own listener: an operator grepping startup logs
		 * for "http target:" should not have to know whether http.route[] was
		 * configured to find every target a gateway may talk to. */
		for (i = 0; i < gw->ntargets; i++) {
			zlog(ZLOG_NOTICE, "[pool %s] http target: pool %s at %s, %u persistent connection(s)",
					wp->config->name, gw->targets[i].pool, gw->targets[i].listen_address,
					gw->targets[i].max_upstreams);
		}

		for (i = 0; i < gw->nproc; i++) {
			gw->slots[i] = calloc(1, sizeof(*gw->slots[i]));
			gw->slots[i]->gw = gw;
			gw->slots[i]->index = i;
			gw->slots[i]->respawn.window_start = time(NULL);
			fpm_http_gateway_spawn(gw, i);
		}
		if (reuseport) {
			/* the master's socket would otherwise take its share of connections and never accept them */
			close(gw->listen_fd);
			gw->listen_fd = -1;
			if (gw->plain_listen_fd >= 0) {
				close(gw->plain_listen_fd);
				gw->plain_listen_fd = -1;
			}
			/* Issue #388: on a TCP gateway gw->listen_fd IS the public
			 * socket (bound through fpm_http_listen() above, never
			 * wp->listening_socket), so closing it here is the whole job --
			 * there is no second, non-REUSEPORT socket to starve the group.
			 * A unix gateway forces reuseport off before it gets here. */
		}
	}

	/* Register cleanup once, when the first http pool is initialized.
	 * PARENT_EXEC too, because reload calls execvp() and without this the
	 * gateways would remain orphaned while holding the port on which the new
	 * master wants to bind. */
	if (!cleanup_registered) {
		if (0 > fpm_cleanup_add(FPM_CLEANUP_PARENT, fpm_http_cleanup, 0) ||
				0 > fpm_cleanup_add(FPM_CLEANUP_PARENT_EXEC, fpm_http_cleanup, 0)) {
			return -1;
		}
		cleanup_registered = 1;
	}
	return 0;
}
/* }}} */

/* Checks specific to the http gateway, called by fpm_pool_type.c while
 * validating the configuration, before anything forks. Serves both pool.type =
 * gateway and (before #388) the retired pool.type = http; the .proxy_only flag
 * decides the few questions whose answer differs. */
int fpm_http_validate_pool(struct fpm_worker_pool_s *wp) /* {{{ */
{
	const struct fpm_pool_type_s *type = fpm_pool_type_of(wp);
	int proxy_only = type->proxy_only;

	/* Issue #388: a gateway runs no process manager of its own, but upstream's
	 * fpm_scoreboard_init_main() refuses to allocate a pool's scoreboard
	 * unless pm_max_children >= 1 ("max_client is not set"), and the operator
	 * page reads that scoreboard for the gateway's baseline counter.
	 * fpm_operator_endpoint_validate() solves the same problem the same way.
	 * programmatically (the `pm` directive is rejected above, so this cannot
	 * be confused with an operator's own setting) and
	 * fpm_children_create_initial() skips proxy_only pools so the slot never
	 * becomes a PHP child. */
	if (proxy_only) {
		wp->config->pm = PM_STYLE_STATIC;
		wp->config->pm_max_children = 1;
	}

	/* Issue #388: the gateway binds its own public listener in
	 * fpm_http_init_pool_ex(), through fpm_http_listen() -- which sets
	 * SO_REUSEPORT (when asked) before bind() and is not in upstream's
	 * sockets_list. Clear the domain so fpm_sockets_init_main() does NOT
	 * create, and later export on exec-reload, a second, non-REUSEPORT socket
	 * for the same address: fpm_sockets switches on listen_address_domain and
	 * 0 matches no case, so the pool is skipped and keeps no master socket.
	 * The address string itself is left in place for the gateway and for
	 * fpm_conf_dump().
	 *
	 * A unix public listener is the exception: fpm_http_listen() parses
	 * host:port only, so a unix gateway keeps the master's socket (and this
	 * domain), exactly as every other listening pool does. That path also
	 * cannot use http.reuseport, which is meaningless on a unix socket. */
	if (proxy_only && wp->listen_address_domain == FPM_AF_INET) {
		wp->listen_address_domain = 0;
	}

	/* Issue #388: on the gateway `listen` is the public HTTP(S) port, so
	 * http.listen has nothing left to override. Refused rather than ignored:
	 * a config that sets both is a config that still believes it is the old
	 * two-socket pool, and silently binding `listen` instead would hide the
	 * migration. */
	if (proxy_only && fpm_conf_directive_was_set(wp->config, "http.listen") && wp->config->http_listen && *wp->config->http_listen) {
		zlog(ZLOG_ERROR, "[pool %s] http.listen is redundant on pool.type = gateway: "
						 "'listen' is the public HTTP(S) port this gateway serves",
				wp->config->name);
		return -1;
	}
	/* Issue #493: listen.allowed_clients is a FastCGI-worker ACL. On every
	 * other listening type it restricts the worker socket -- on the retired
	 * combined `http` pool it restricted the FastCGI half, never the public
	 * port, which used http.allowed_clients. A gateway has no worker socket:
	 * `listen` IS the public port, and accepting the directive would leave an
	 * operator who wrote it believing the public listener was restricted while
	 * it served everyone. Refused by name with the replacement rather than
	 * ignored; http.allowed_clients is the ACL that actually guards this
	 * listener (see fpm_http_gateway_settings()). */
	if (proxy_only && fpm_conf_directive_was_set(wp->config, "listen.allowed_clients") && wp->config->listen_allowed_clients && *wp->config->listen_allowed_clients) {
		zlog(ZLOG_ERROR, "[pool %s] listen.allowed_clients is not enforced on pool.type = gateway: "
						 "it is a FastCGI-worker ACL and a gateway runs no worker; use http.allowed_clients to "
						 "restrict this gateway's public listener",
				wp->config->name);
		return -1;
	}
	/* Issue #388: with no implicit own-pool target there is nothing to serve
	 * an unrouted request with, so a gateway with no http.route[] at all is a
	 * configuration error rather than a proxy that answers 404 to everything.
	 * (A gateway whose routes simply do not claim "/" is allowed -- see the
	 * startup NOTICE in fpm_http_routes_build().) */
	if (proxy_only && !wp->config->http_routes) {
		zlog(ZLOG_ERROR, "[pool %s] pool.type = gateway with no http.route[] serves nothing; "
						 "add at least one 'http.route[<pool>] = <prefix>'",
				wp->config->name);
		return -1;
	}

	if (fpm_conf_directive_was_set(wp->config, "http.gateways") && wp->config->http_gateways < 1) {
		zlog(ZLOG_ERROR, "[pool %s] http.gateways must be at least 1", wp->config->name);
		return -1;
	}
	if (fpm_conf_directive_was_set(wp->config, "http.idle_timeout") && wp->config->http_idle_timeout < 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.idle_timeout must not be negative", wp->config->name);
		return -1;
	}
	if (wp->config->http_read_timeout < 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.read_timeout must not be negative", wp->config->name);
		return -1;
	}
	if (wp->config->http_keepalive_timeout < 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.keepalive_timeout must not be negative", wp->config->name);
		return -1;
	}
	if (wp->config->http_write_timeout < 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.write_timeout must not be negative", wp->config->name);
		return -1;
	}
	/* issue #340. Deliberately here and not in fpm_http_routes_build(): the
	 * route table names OTHER pools, and by the time every pool has been
	 * validated they have all been parsed, so a typo in a pool name is a
	 * startup refusal (and a `-t` failure) rather than a 502 per request on a
	 * prefix the operator believes is configured. */
	if (fpm_http_validate_routes(wp) != 0) {
		return -1;
	}
	/* Issue #389: http.operator. With both base paths empty there is nothing to
	 * forward under, so the switch cannot mean anything; the ACL is required
	 * because these pages describe the inside of the process tree and the
	 * public port is not loopback. A route claiming a base OR any path under
	 * one is refused: operator paths are matched before routing, so such a
	 * route could never fire -- a silent no-op the operator would report as a
	 * bug. */
	if (proxy_only && wp->config->http_operator) {
		char metrics_scratch[192], status_scratch[192];
		const char *mbase = fpm_http_operator_base(wp, 1, metrics_scratch, sizeof(metrics_scratch));
		const char *sbase = fpm_http_operator_base(wp, 0, status_scratch, sizeof(status_scratch));
		const char *raw = wp->config->http_operator_allowed_clients;
		struct fpm_http_acl_s *acl = NULL;
		struct key_value_s *kv;

		/* Parse first, and require a value that actually PARSES TO ENTRIES.
		 * fpm_http_acl_parse() returns success with *out == NULL for a
		 * non-empty string that names no address (",", " "). Treating that as
		 * "an ACL was given" would leave the forwarded pages world-readable,
		 * because a NULL ACL makes the runtime guard
		 * (`gw->operator_acl && !fpm_http_acl_check(...)`) short-circuit. */
		if (raw && *raw &&
				fpm_http_acl_parse(wp->config->name, "http.operator_allowed_clients", raw, &acl) != 0) {
			return -1; /* a bad address: parse already logged which one */
		}
		if (!acl) {
			if (!raw || !*raw) {
				zlog(ZLOG_ERROR, "[pool %s] http.operator = yes requires http.operator_allowed_clients: "
								 "the forwarded pages describe the inside of the process tree, so who may reach them on "
								 "the public port has to be stated (issue #389)",
						wp->config->name);
			} else {
				zlog(ZLOG_ERROR, "[pool %s] http.operator_allowed_clients = '%s' names no address, so it "
								 "is not an ACL and would leave the forwarded pages world-readable; http.operator = yes "
								 "requires at least one (issue #389)",
						wp->config->name, raw);
			}
			return -1;
		}
		fpm_http_acl_free(acl);

		if (!mbase && !sbase) {
			zlog(ZLOG_ERROR, "[pool %s] http.operator = yes but both operator.metrics_path and "
							 "operator.status_path are empty, so there is no base to forward <base>/<pool> under "
							 "(issue #389)",
					wp->config->name);
			return -1;
		}

		for (kv = wp->config->http_routes; kv; kv = kv->next) {
			const char *cursor = kv->value, *prefix;
			size_t prefix_len;

			while (fpm_http_route_next_prefix(&cursor, &prefix, &prefix_len)) {
				const char *collide = NULL;
				size_t base_len;

				/* At or UNDER the base: "operator paths are checked before
				 * http.route[]", so /metrics/app is answered by the map (or a
				 * local 404), never by a route. Segment-aware, exactly like
				 * fpm_http_operator_under_base(): /metricsx is a different
				 * prefix and is not shadowed. */
				if (mbase) {
					base_len = strlen(mbase);
					if (prefix_len >= base_len && !memcmp(prefix, mbase, base_len) && (prefix_len == base_len || prefix[base_len] == '/')) {
						collide = mbase;
					}
				}
				if (!collide && sbase) {
					base_len = strlen(sbase);
					if (prefix_len >= base_len && !memcmp(prefix, sbase, base_len) && (prefix_len == base_len || prefix[base_len] == '/')) {
						collide = sbase;
					}
				}
				if (collide) {
					zlog(ZLOG_ERROR, "[pool %s] http.route[%s]: path prefix '%.*s' falls in the operator "
									 "namespace of base path '%s'; with http.operator = yes the gateway answers "
									 "<base>/<pool> there before routing, so a route cannot claim it (issue #389)",
							wp->config->name, kv->key, (int) prefix_len, prefix, collide);
					return -1;
				}
			}
		}
	} else if (wp->config->http_operator_allowed_clients && *wp->config->http_operator_allowed_clients) {
		/* http.operator off: the addresses are still validated, so a typo in a
		 * directive the operator wrote fails `-t` instead of sitting unread. */
		struct fpm_http_acl_s *tmp = NULL;

		if (fpm_http_acl_parse(wp->config->name, "http.operator_allowed_clients",
					wp->config->http_operator_allowed_clients, &tmp) != 0) {
			return -1;
		}
		fpm_http_acl_free(tmp);
	}
	/* Issue #388: http.front_controller is the fallback for a request whose
	 * path names no file, and on the gateway it applies to every target. When
	 * no route claims "/" it is still meaningful (an API-only gateway with a
	 * front controller under /api), so this is a NOTICE and not a refusal --
	 * the decision the issue left open. It is worth saying because a front
	 * controller set in a copied http section is the commonest thing an
	 * operator forgets when moving to the gateway shape. */
	if (proxy_only && wp->config->http_front_controller && *wp->config->http_front_controller) {
		struct key_value_s *kv;
		int root_claimed = 0;

		for (kv = wp->config->http_routes; kv && !root_claimed; kv = kv->next) {
			const char *cursor = kv->value, *prefix;
			size_t prefix_len;

			while (fpm_http_route_next_prefix(&cursor, &prefix, &prefix_len)) {
				if (prefix_len == 1 && prefix[0] == '/') {
					root_claimed = 1;
					break;
				}
			}
		}
		if (!root_claimed) {
			zlog(ZLOG_NOTICE, "[pool %s] gateway: http.front_controller is set but no "
							  "http.route[] entry claims '/', so the fallback is only reachable under a "
							  "routed prefix",
					wp->config->name);
		}
	}
	/* http.pool_full_policy = wait (issue #309): both bounds are mandatory
	 * whenever the policy is on, the same rule the throwaway spike (#155)
	 * enforced by silently falling back to reject. Refused loudly here
	 * instead: an unbounded queue is the hoarding hazard the 503 exists to
	 * avoid, and an unbounded wait is the invisible one. Checked regardless
	 * of whether the two integer directives were explicitly set, because the
	 * shipped defaults (32 / 500ms, see docs/http-gateway-pool-full.md) are
	 * only sane in combination -- a pool that zeroes one out while turning
	 * the policy on must not end up with the other silently unbounded too. */
	if (wp->config->http_pool_full_policy == FPM_HTTP_POOL_FULL_WAIT) {
		if (wp->config->http_pool_full_queue_max <= 0) {
			zlog(ZLOG_ERROR, "[pool %s] http.pool_full_policy = wait requires http.pool_full_queue_max > 0", wp->config->name);
			return -1;
		}
		if (wp->config->http_pool_full_wait_ms <= 0) {
			zlog(ZLOG_ERROR, "[pool %s] http.pool_full_policy = wait requires http.pool_full_wait_ms > 0", wp->config->name);
			return -1;
		}
	}
	/* Issue #388: the old "a unix-socket pool requires http.listen, there is
	 * no FastCGI port to bump by one" check is gone with the type it guarded.
	 * On the gateway `listen` IS the public address and may itself be a unix
	 * socket (a front end that speaks HTTP to it), so there is nothing to
	 * require. */
	if (wp->config->http_allowed_clients && *wp->config->http_allowed_clients) {
		struct fpm_http_acl_s *tmp = NULL;

		if (fpm_http_acl_parse(wp->config->name, "http.allowed_clients", wp->config->http_allowed_clients, &tmp) != 0) {
			return -1; /* fpm_http_acl_parse() already logged which address is bad */
		}
		fpm_http_acl_free(tmp);
	}
	if (wp->config->http_trusted_proxies && *wp->config->http_trusted_proxies) {
		struct fpm_http_acl_s *tmp = NULL;

		if (fpm_http_acl_parse(wp->config->name, "http.trusted_proxies", wp->config->http_trusted_proxies, &tmp) != 0) {
			return -1; /* fpm_http_acl_parse() already logged which address is bad */
		}
		fpm_http_acl_free(tmp);
	}
	if (wp->config->http_front_controller && *wp->config->http_front_controller) {
		const char *fc = wp->config->http_front_controller;
		size_t len = strlen(fc);

		if (fc[0] != '/' || strstr(fc, "/../") || (len >= 3 && !strcmp(fc + len - 3, "/.."))) {
			zlog(ZLOG_ERROR, "[pool %s] http.front_controller must be an absolute path under the document root, without '..'", wp->config->name);
			return -1;
		}
	}
	if (wp->config->http_tls_wait_for_cert) {
#ifdef HAVE_FPM_HTTP_TLS
		/* The paths must still be configured -- they are what the master
		 * watches. "Wait for a certificate" without being told where it will
		 * appear has no meaning, and silently accepting it would leave a pool
		 * in NO_CERT forever. */
		if (!wp->config->http_tls_cert || !*wp->config->http_tls_cert) {
			zlog(ZLOG_ERROR, "[pool %s] http.tls_wait_for_cert needs http.tls_cert: it relaxes when the certificate has to exist, not whether a path is configured", wp->config->name);
			return -1;
		}
		/* http.tls_sni_cert is validated and loaded only as part of the
		 * startup pair, and is explicitly not part of the reload poll (see
		 * docs/acme-renewal.md). A pool that starts in NO_CERT skips that
		 * load entirely, so after the transition it would serve the primary
		 * certificate for every SNI name -- wrong, and silent, because the
		 * validation that would have complained was skipped too. Refuse the
		 * combination rather than ship the wrong certificate. */
		if (wp->config->http_tls_sni_cert && *wp->config->http_tls_sni_cert) {
			zlog(ZLOG_ERROR, "[pool %s] http.tls_wait_for_cert cannot be combined with http.tls_sni_cert: SNI certificates are loaded once at startup and are not part of the certificate-watch poll, so a pool that started without them would never pick them up", wp->config->name);
			return -1;
		}
		/* http.tls_reload_check is the only mechanism that notices the
		 * certificate appearing, so turning it off turns NO_CERT into a
		 * permanent state. Refusing the combination is better than a pool
		 * that starts cleanly and never serves. */
		if (fpm_conf_directive_was_set(wp->config, "http.tls_reload_check") &&
				wp->config->http_tls_reload_check <= 0) {
			zlog(ZLOG_ERROR, "[pool %s] http.tls_wait_for_cert requires http.tls_reload_check to be on: with it at 0 nothing would ever notice the certificate appearing and the TLS listener would stay closed for the life of the master", wp->config->name);
			return -1;
		}
#else
		zlog(ZLOG_ERROR, "[pool %s] http.tls_wait_for_cert requires php-fpm-ng to be built with TLS support: "
						 "rebuild with ./configure --enable-fpmng-tls (needs libevent_openssl and OpenSSL)",
				wp->config->name);
		return -1;
#endif
	}
	if (wp->config->http_plain_listen && *wp->config->http_plain_listen &&
			(!wp->config->http_tls_cert || !*wp->config->http_tls_cert)) {
		zlog(ZLOG_ERROR, "[pool %s] http.plain_listen requires http.tls_cert", wp->config->name);
		return -1;
	}
	if (wp->config->http_tls_cert && *wp->config->http_tls_cert) {
#ifdef HAVE_FPM_HTTP_TLS
		if (!wp->config->http_tls_key || !*wp->config->http_tls_key) {
			zlog(ZLOG_ERROR, "[pool %s] http.tls_cert requires http.tls_key", wp->config->name);
			return -1;
		}
		/* Reads cert+key from disk into a throwaway SSL_CTX and checks they
		 * parse and match -- a bad path or a mismatched key must fail here,
		 * before fpm_http_init_pool_ex() forks a single gateway child, not
		 * as a crash or a silent plain-HTTP fallback at request time.
		 *
		 * Under http.tls_wait_for_cert the check is skipped only when the
		 * certificate is NOT THERE (issue #172). A file that exists and does
		 * not parse, or a key that does not match its certificate, still
		 * fails startup exactly as before: that is an operator error, and
		 * treating it as "not issued yet" would turn every typo into a pool
		 * that quietly refuses connections on :443 forever. The distinction
		 * is made by access(), not by the validate's return value, precisely
		 * so that the two failure modes cannot be confused.
		 *
		 * The key is checked too, not just the certificate: a half-finished
		 * install with only one of the two on disk is "not issued yet", not
		 * a broken configuration. Our own installer writes the key first and
		 * the chain second (sapi/fpmng/acme/state.php,
		 * installCertificate()), so the window is real but narrow. */
		if (!(wp->config->http_tls_wait_for_cert &&
					(access(wp->config->http_tls_cert, R_OK) != 0 ||
							!wp->config->http_tls_key || access(wp->config->http_tls_key, R_OK) != 0)) &&
				fpm_tls_http_validate(wp->config->name, wp->config->http_tls_cert, wp->config->http_tls_key,
						wp->config->http_tls_min_version, wp->config->http_tls_sni_cert,
						wp->config->http_tls_verify_client, wp->config->http_tls_client_ca) != 0) {
			return -1; /* fpm_tls_http_validate() already logged what is wrong */
		}
#else
		zlog(ZLOG_ERROR, "[pool %s] http.tls_cert requires php-fpm-ng to be built with TLS support: "
						 "rebuild with ./configure --enable-fpmng-tls (needs libevent_openssl and OpenSSL)",
				wp->config->name);
		return -1;
#endif
	} else if (wp->config->http_tls_key && *wp->config->http_tls_key) {
		zlog(ZLOG_ERROR, "[pool %s] http.tls_key without http.tls_cert has nothing to attach the key to", wp->config->name);
		return -1;
	} else if (wp->config->http_tls_verify_client && *wp->config->http_tls_verify_client &&
			   strcmp(wp->config->http_tls_verify_client, "none") != 0) {
		zlog(ZLOG_ERROR, "[pool %s] http.tls_verify_client without http.tls_cert has no TLS handshake to request a client certificate on", wp->config->name);
		return -1;
	}
	return 0;
}
/* }}} */

int fpm_http_init_pool(struct fpm_worker_pool_s *wp) /* {{{ */
{
	return fpm_http_init_pool_ex(wp, 0);
}
/* }}} */

int fpm_http_init_pool_with_capacity(struct fpm_worker_pool_s *wp, unsigned capacity) /* {{{ */
{
	return fpm_http_init_pool_ex(wp, capacity);
}
/* }}} */

/* Issue #341/#390: the gateway behind one pool name, or NULL. Runs in the
 * operator endpoint's OWN child (see fpm_http.h), which reached this point by
 * fork()ing the master AFTER fpm_http_init_pool_ex() built the `gateways` list
 * -- so this process has its own copy of the list, pointing at the same shared
 * counters segment every gateway process of the pool updates. `gateways` is a
 * flat list across every gateway pool in the config (one entry per pool, not
 * per gateway process, see fpm_http_gateway_s's comment), hence the name match
 * instead of a pointer this file never handed out. */
static struct fpm_http_gateway_s *fpm_http_gateway_find(const char *name) /* {{{ */
{
	struct fpm_http_gateway_s *gw;

	for (gw = gateways; gw; gw = gw->next) {
		if (strcmp(gw->pool, name) == 0) {
			return gw;
		}
	}
	return NULL;
}
/* }}} */

/* Issue #390: the label of slot i -- a routed pool's own name, "operator" for
 * the row #389's forwarded pages use, "-" for everything the gateway answered
 * itself. The two literals are the same ones the access log has always used
 * ("operator" from #389, "-" from #341), so a scraper and a log line name the
 * same bucket. */
static const char *fpm_http_counters_slot_label(struct fpm_http_gateway_s *gw, unsigned i) /* {{{ */
{
	if (i < gw->ntargets) {
		return gw->targets[i].pool;
	}
	return i == gw->ntargets ? "operator" : "-";
}
/* }}} */

/* Issue #390: the fixed part of one slot's row, for both the metrics renderer
 * and the status renderer -- the same reason fpm_operator_pages.c funnels both
 * formats through one collect(): a field added for one cannot go missing from
 * the other. Rejected is spelled rejected_total out of #341; #390's prose calls
 * it "rejected_503_total" because that is what it counts (budget exhausted,
 * answered 503). */
struct fpm_http_gateway_row_s {
	const char *target;
	unsigned long requests;
	unsigned long rejected;
	unsigned long upstreams_used;
	unsigned max_upstreams;
};

static void fpm_http_counters_row(struct fpm_http_gateway_s *gw, unsigned i,
		struct fpm_http_gateway_row_s *out)
{
	atomic_t *slot = fpm_http_counters_slot_cells(gw->counters, i);
	unsigned p;

	out->target = fpm_http_counters_slot_label(gw, i);
	out->requests = (unsigned long) slot[0];
	out->rejected = (unsigned long) slot[1];
	/* The gauge, not the shared budget: sum what every gateway process
	 * currently holds for this row, so a dead process's connections (its
	 * block was zeroed by the master) are no longer in the number. */
	out->upstreams_used = 0;
	for (p = 0; p < gw->counters->nproc; p++) {
		out->upstreams_used += (unsigned long) fpm_http_counters_gauges(gw->counters, p)[1 + i];
	}
	out->max_upstreams = i < gw->ntargets ? gw->targets[i].max_upstreams
										  : (i == gw->ntargets ? FPM_HTTP_OPERATOR_UPSTREAMS : 0);
}

/* Issue #390: the /metrics index -- one line per pool the gateway FORWARDS for
 * (#389). Not an aggregate of their series (that endpoint was removed in #278);
 * it is a discovery aid, so a scraper that found the gateway knows where
 * <base>/<pool> points. The gateway's own page is skipped: it is served at the
 * bare base, not forwarded. A pool that exposed only one of the two formats
 * gets an empty string in the other attribute rather than a URL that 404s. */
static void fpm_http_render_exposed_pools(struct fpm_http_gateway_s *gw, struct fpm_operator_buf_s *b) /* {{{ */
{
	unsigned i, j;
	int header = 0;

	for (i = 0; i < gw->noperator_entries; i++) {
		struct fpm_http_operator_entry_s *e = &gw->operator_entries[i];
		const char *metrics_path = NULL, *status_path = NULL;

		if (!e->pool || e->own) {
			continue;
		}
		/* One line per POOL: skip a pool whose first entry was already
		 * emitted, whatever order the two formats came in. */
		for (j = 0; j < i; j++) {
			if (!gw->operator_entries[j].own && gw->operator_entries[j].pool == e->pool) {
				break;
			}
		}
		if (j < i) {
			continue;
		}
		for (j = 0; j < gw->noperator_entries; j++) {
			struct fpm_http_operator_entry_s *o = &gw->operator_entries[j];

			if (o->own || o->pool != e->pool) {
				continue;
			}
			if (o->metrics) {
				metrics_path = o->path;
			} else {
				status_path = o->path;
			}
		}
		if (!header) {
			fpm_operator_buf_appendf(b,
					"# HELP fpmng_gateway_exposed_pool Pools this gateway forwards operator pages for, and their public paths.\n"
					"# TYPE fpmng_gateway_exposed_pool gauge\n");
			header = 1;
		}
		fpm_operator_buf_appendf(b,
				"fpmng_gateway_exposed_pool{pool=\"%s\",metrics=\"%s\",status=\"%s\"} 1\n",
				e->pool, metrics_path ? metrics_path : "", status_path ? status_path : "");
	}
}
/* }}} */

/* Issue #390: fpm_pool_type_s.baseline for pool.type = gateway -- the pool's
 * accepted-request total, read from the segment instead of the scoreboard no
 * gateway child bumps. Called from the operator endpoint's own child, shared
 * memory only. */
unsigned long fpm_http_gateway_baseline_requests(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_http_gateway_s *gw = fpm_http_gateway_find(wp->config->name);

	return (gw && gw->counters) ? (unsigned long) gw->counters->requests_total : 0UL;
}
/* }}} */

/* Issue #341, fpm_pool_type_s.render_metrics_prometheus for pool.type = gateway.
 * Runs in the operator endpoint's OWN child (see fpm_http.h) and reads the one
 * segment fpm_http_routes_build() allocated before any child forked. */
void fpm_http_render_metrics_prometheus(struct fpm_worker_pool_s *wp, struct fpm_operator_buf_s *b) /* {{{ */
{
	struct fpm_http_gateway_s *gw = fpm_http_gateway_find(wp->config->name);
	unsigned i;

	if (!gw || !gw->counters || !gw->ntargets) {
		return;
	}

	fpm_operator_buf_appendf(b,
			"# HELP fpmng_gateway_upstreams_used Persistent connections this gateway currently holds open to a target.\n"
			"# TYPE fpmng_gateway_upstreams_used gauge\n"
			"# HELP fpmng_gateway_upstreams_max Persistent connections this gateway may hold open to a target, from the target's own pm.max_children.\n"
			"# TYPE fpmng_gateway_upstreams_max gauge\n"
			"# HELP fpmng_gateway_requests_total Requests this gateway accepted for a target, however they were answered.\n"
			"# TYPE fpmng_gateway_requests_total counter\n"
			"# HELP fpmng_gateway_rejected_total Of those, how many found no free connection and no budget and were answered 503.\n"
			"# TYPE fpmng_gateway_rejected_total counter\n"
			"# HELP fpmng_gateway_connections_open Client connections currently open to the gateway.\n"
			"# TYPE fpmng_gateway_connections_open gauge\n"
			"# HELP fpmng_gateway_ping_total ping.path answers, served by the gateway itself.\n"
			"# TYPE fpmng_gateway_ping_total counter\n");

	fpm_operator_buf_appendf(b,
			"fpmng_gateway_connections_open{pool=\"%s\"} %lu\n"
			"fpmng_gateway_ping_total{pool=\"%s\"} %lu\n",
			gw->pool, fpm_http_connections_open(gw),
			gw->pool, (unsigned long) gw->counters->ping_total);

	for (i = 0; i < gw->counters->nslots; i++) {
		struct fpm_http_gateway_row_s row;

		fpm_http_counters_row(gw, i, &row);
		fpm_operator_buf_appendf(b,
				"fpmng_gateway_upstreams_used{pool=\"%s\",target=\"%s\"} %lu\n"
				"fpmng_gateway_upstreams_max{pool=\"%s\",target=\"%s\"} %u\n"
				"fpmng_gateway_requests_total{pool=\"%s\",target=\"%s\"} %lu\n"
				"fpmng_gateway_rejected_total{pool=\"%s\",target=\"%s\"} %lu\n",
				gw->pool, row.target, row.upstreams_used,
				gw->pool, row.target, row.max_upstreams,
				gw->pool, row.target, row.requests,
				gw->pool, row.target, row.rejected);
	}

	fpm_http_render_exposed_pools(gw, b);
}
/* }}} */

/* Issue #390: fpm_pool_type_s.operator_status for pool.type = gateway -- the
 * same numbers as the metrics page, in the shape of the generic per-pool status
 * page so a client that parses {"pools":[...]} keeps working.
 *
 * The first row is the pool itself (the two numbers no target owns: open client
 * connections and pings), then one row per target label, so "one row per
 * target" holds and the pool-wide values are still reachable. Target names are
 * the same literals the metrics page labels with. */
void fpm_http_gateway_operator_status(struct fpm_worker_pool_s *wp, const char *query,
		struct fpm_operator_reply_s *reply) /* {{{ */
{
	struct fpm_http_gateway_s *gw = fpm_http_gateway_find(wp->config->name);
	unsigned i;

	(void) query;
	reply->content_type = "application/json";

	if (!gw || !gw->counters) {
		fpm_operator_buf_appendf(&reply->body, "{\"pools\":[]}\n");
		reply->handled = 1;
		return;
	}

	fpm_operator_buf_appendf(&reply->body,
			"{\"pools\":[{\"name\":\"%s\",\"type\":\"gateway\",\"serves_requests\":false,"
			"\"requests\":%lu,\"connections_open\":%lu,\"ping_total\":%lu}",
			gw->pool, (unsigned long) gw->counters->requests_total,
			fpm_http_connections_open(gw),
			(unsigned long) gw->counters->ping_total);

	for (i = 0; i < gw->counters->nslots; i++) {
		struct fpm_http_gateway_row_s row;

		fpm_http_counters_row(gw, i, &row);
		fpm_operator_buf_appendf(&reply->body,
				",{\"name\":\"%s\",\"type\":\"gateway\",\"serves_requests\":false,"
				"\"target\":\"%s\",\"requests\":%lu,\"rejected_503\":%lu,\"upstreams_used\":%lu}",
				gw->pool, row.target, row.requests, row.rejected, row.upstreams_used);
	}
	fpm_operator_buf_appendf(&reply->body, "]}\n");
	reply->handled = 1;
}
/* }}} */

#endif /* HAVE_FPM_HTTP */
