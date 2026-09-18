/* fpm-ng: the per-pool operator endpoint. See fpm_operator_endpoint.h for the
 * model and for why the answering process is an internal pool rather than the
 * master or one of the pool's own children.
 */

#include "fpm_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_operator_endpoint.h"
#include "fpm_operator_http.h"
#include "fpm_operator_pages.h"
#include "fpm_pool_type.h"
#include "zlog.h"

#define FPM_OPERATOR_ENDPOINT_DEFAULT_LISTEN "127.0.0.1:8080"

const char *const fpm_operator_endpoint_directives[] = {
	"pm.status_path",
	"pm.status_listen",
	"pm.metrics_path",
	"pm.metrics_listen",
	NULL
};

/* The internal listener pool runs no PHP, reads no request and has no FastCGI
 * transport, so everything that describes one is meaningless on it. It is not
 * user-configurable in the first place -- this list exists so that a future
 * change which starts copying directives onto it fails loudly instead of
 * quietly carrying one that does nothing. */
const char *const fpm_operator_endpoint_rejects[] = {
	"pm",
	"pm.",
	"request_terminate_timeout",
	"request_terminate_timeout_track_finished",
	"request_slowlog_timeout",
	"request_slowlog_trace_depth",
	"slowlog",
	"ping.",
	"access.",
	"security.limit_extensions",
	"supervisor.",
	"cron.",
	"http.",
	"fiber.",
	"worker.",
	NULL
};

enum fpm_operator_format_e {
	FPM_OPERATOR_FORMAT_JSON = 0,	/* pm.status_path */
	FPM_OPERATOR_FORMAT_PROMETHEUS	/* pm.metrics_path */
};

struct fpm_operator_route_s {
	char *path;
	const char *directive;			/* which directive put it here, for the collision message */
	enum fpm_operator_format_e format;
	struct fpm_worker_pool_s *pool;		/* the pool this route reports on */
	struct fpm_operator_route_s *next;
};

struct fpm_operator_listener_s {
	char *address;				/* as configured, see the note in _find() */
	struct fpm_worker_pool_s *wp;		/* the internal pool that binds it */
	struct fpm_worker_pool_s *first;	/* the pool whose identity wp runs with */
	struct fpm_operator_route_s *routes;
	char *known_paths;			/* for the 404 body; rebuilt as routes are added */
	struct fpm_operator_listener_s *next;
};

/* Built in the master before any fork, then read-only. Children inherit it
 * through fork(), which is why nothing here has to live in shared memory. */
static struct fpm_operator_listener_s *fpm_operator_listeners = NULL;

int fpm_operator_endpoint_validate(struct fpm_worker_pool_s *wp) /* {{{ */
{
	/* One process, for the same reason pool.type = status had one: this is a
	 * sequential HTTP server for monitoring scrapes every 15-60 seconds, not
	 * public traffic. There is deliberately no directive to raise it -- if more
	 * were ever needed that is a design decision, not a default. */
	wp->config->pm = PM_STYLE_STATIC;
	wp->config->pm_max_children = 1;

	return 0;
}
/* }}} */

/* A path an operator endpoint will answer on. The rules are upstream's for
 * pm.status_path, kept identical so that a path which was legal before the
 * meaning change in #278 is still legal after it. */
static int fpm_operator_endpoint_check_path(struct fpm_worker_pool_s *wp, const char *directive,
	const char *path) /* {{{ */
{
	size_t i, len = strlen(path);

	if (*path != '/') {
		zlog(ZLOG_ERROR, "[pool %s] the %s '%s' must start with a '/'",
			wp->config->name, directive, path);
		return -1;
	}
	if (len < 2) {
		zlog(ZLOG_ERROR, "[pool %s] the %s '%s' is not long enough",
			wp->config->name, directive, path);
		return -1;
	}
	for (i = 0; i < len; i++) {
		if (!isalnum((unsigned char) path[i]) && path[i] != '/' && path[i] != '-'
			&& path[i] != '_' && path[i] != '.' && path[i] != '~') {
			zlog(ZLOG_ERROR, "[pool %s] the %s '%s' must contain only the following "
				"characters '[alphanum]/_-.~'", wp->config->name, directive, path);
			return -1;
		}
	}
	return 0;
}
/* }}} */

/* The identity an operator listener runs with, and the identity of the socket
 * it binds. fpm_conf_internal_pool_alloc() copies these from the pool that
 * asked for the listener first; every pool that later names the same address
 * shares that one process and contributes nothing.
 *
 * While the pools agree that is invisible. When they do not it is indefensible,
 * and silent: pool 'b' would be reported on by a process running as pool 'a's
 * user, and with a unix-socket address 'b's listen.group and listen.mode would
 * be ignored, so 'b's scraper gets EACCES with nothing in the log to explain
 * it. Since the default address is the same for every pool, sharing a listener
 * is the normal case rather than an unusual one, so a disagreement is refused
 * at startup instead of warned about. */
static const struct {
	const char *directive;
	size_t offset;
} fpm_operator_identity[] = {
	{ "user",         offsetof(struct fpm_worker_pool_config_s, user) },
	{ "group",        offsetof(struct fpm_worker_pool_config_s, group) },
	{ "listen.owner", offsetof(struct fpm_worker_pool_config_s, listen_owner) },
	{ "listen.group", offsetof(struct fpm_worker_pool_config_s, listen_group) },
	{ "listen.mode",  offsetof(struct fpm_worker_pool_config_s, listen_mode) },
};

static int fpm_operator_listener_identity_ok(struct fpm_operator_listener_s *l,
	struct fpm_worker_pool_s *wp) /* {{{ */
{
	size_t i;

	for (i = 0; i < sizeof(fpm_operator_identity) / sizeof(fpm_operator_identity[0]); i++) {
		char *const *mine = (char *const *) ((char *) wp->config + fpm_operator_identity[i].offset);
		char *const *theirs = (char *const *) ((char *) l->first->config + fpm_operator_identity[i].offset);

		if (!*mine && !*theirs) {
			continue;
		}
		if (*mine && *theirs && !strcmp(*mine, *theirs)) {
			continue;
		}

		zlog(ZLOG_ALERT, "[pool %s] shares the operator listener %s with pool '%s', but the "
			"two disagree on %s ('%s' against '%s') -- one listener is one process and one "
			"socket, so it can only have one identity; give this pool its own listen address "
			"or make the two agree",
			wp->config->name, l->address, l->first->config->name,
			fpm_operator_identity[i].directive,
			*mine ? *mine : "(unset)", *theirs ? *theirs : "(unset)");
		return -1;
	}

	return 0;
}
/* }}} */

/* Listener for this address, creating it (and its internal pool) on first use.
 *
 * Addresses are compared as configured, so "localhost:8080" and
 * "127.0.0.1:8080" are two listeners rather than one. That is deliberate: the
 * alternative is resolving names at configuration time and calling two spellings
 * equal on the strength of it, and the failure mode of being wrong there is
 * silently merging two operators' endpoints. Being wrong the way it is now
 * costs a bind() failure at startup, with the address in the message. */
static struct fpm_operator_listener_s *fpm_operator_listener_get(struct fpm_worker_pool_s *wp,
	const char *address) /* {{{ */
{
	struct fpm_operator_listener_s *l;
	struct fpm_worker_pool_s *lwp;
	char name[192];

	for (l = fpm_operator_listeners; l; l = l->next) {
		if (!strcmp(l->address, address)) {
			return l;
		}
	}

	l = calloc(1, sizeof(*l));
	if (!l) {
		return NULL;
	}
	l->address = strdup(address);
	if (!l->address) {
		free(l);
		return NULL;
	}

	/* The name is what an operator sees in this listener's log lines, so it
	 * says what the pool is and where it listens rather than carrying the name
	 * of whichever pool happened to ask for it first -- several pools can share
	 * one listener and the first is not more entitled to name it. */
	snprintf(name, sizeof(name), "__operator %s", address);

	lwp = fpm_conf_internal_pool_alloc(name, "operator-endpoint", address, wp);
	if (!lwp) {
		free(l->address);
		free(l);
		return NULL;
	}
	l->wp = lwp;
	l->first = wp;

	l->next = fpm_operator_listeners;
	fpm_operator_listeners = l;

	return l;
}
/* }}} */

static void fpm_operator_listener_note_path(struct fpm_operator_listener_s *l, const char *path) /* {{{ */
{
	size_t have = l->known_paths ? strlen(l->known_paths) : 0;
	char *grown = realloc(l->known_paths, have + strlen(path) + 2);

	if (!grown) {
		return;
	}
	if (!have) {
		grown[0] = '\0';
	} else {
		strcat(grown, " ");
	}
	strcat(grown, path);
	l->known_paths = grown;
}
/* }}} */

static int fpm_operator_endpoint_add_route(struct fpm_worker_pool_s *wp, const char *directive,
	const char *listen_address, const char *path, enum fpm_operator_format_e format) /* {{{ */
{
	struct fpm_operator_listener_s *l;
	struct fpm_operator_route_s *r, *tail;

	if (0 > fpm_operator_endpoint_check_path(wp, directive, path)) {
		return -1;
	}

	l = fpm_operator_listener_get(wp, listen_address);
	if (!l) {
		zlog(ZLOG_ERROR, "[pool %s] failed to create the operator listener for %s",
			wp->config->name, listen_address);
		return -1;
	}

	/* Joining an existing listener rather than creating one: it is already
	 * running as some pool's user and, on a unix socket, owned by some pool's
	 * group. It has to be this pool's too. */
	if (0 > fpm_operator_listener_identity_ok(l, wp)) {
		return -1;
	}

	/* The collision rule from #273, point 7: the triple (address, port, path).
	 * Two pools may share an address as long as their paths differ, and one pool
	 * may serve both formats from one address. What is refused is two answers
	 * for one URL, because the operator scraping it has no way to tell which
	 * pool replied. */
	for (r = l->routes; r; r = r->next) {
		if (strcmp(r->path, path) != 0) {
			continue;
		}

		/* The commonest way to hit this is one pool pointing both of its
		 * directives at the same path, which is a different mistake from two
		 * pools colliding and deserves a message that names both directives
		 * rather than telling an operator their pool collides with itself. */
		if (r->pool == wp) {
			zlog(ZLOG_ALERT, "[pool %s] %s and %s are both %s on %s -- one path answers with "
				"one thing, so the status page and the metrics page need different paths "
				"or different listen addresses",
				wp->config->name, r->directive, directive, path, listen_address);
			return -1;
		}

		zlog(ZLOG_ALERT, "[pool %s] %s = %s collides with pool '%s', which already "
			"answers that path on %s -- an address, a port and a path identify one "
			"endpoint, so give this one a different path or a different listen address",
			wp->config->name, directive, path, r->pool->config->name, listen_address);
		return -1;
	}

	r = calloc(1, sizeof(*r));
	if (!r) {
		return -1;
	}
	r->path = strdup(path);
	if (!r->path) {
		free(r);
		return -1;
	}
	r->directive = directive;
	r->format = format;
	r->pool = wp;

	/* Appended, not prepended: the order routes are matched in is the order they
	 * were configured in, which is the order an operator reading fpm.conf
	 * expects. Nothing depends on it today -- paths are unique on a listener, so
	 * at most one can match -- but a later change that relaxes matching should
	 * inherit the obvious order rather than a reversed one. */
	if (!l->routes) {
		l->routes = r;
	} else {
		for (tail = l->routes; tail->next; tail = tail->next) {
			/* find the end */
		}
		tail->next = r;
	}

	fpm_operator_listener_note_path(l, path);

	return 0;
}
/* }}} */

int fpm_operator_endpoint_configure(struct fpm_worker_pool_s *wp, const struct fpm_pool_type_s *type) /* {{{ */
{
	const char *status_path = wp->config->pm_status_path;
	const char *metrics_path = wp->config->pm_metrics_path;

	if (!type->operator_endpoint) {
		return 0;
	}

	/* #273, point 4: no on/off directive. The endpoint exists iff a path is
	 * set, so a pool that configured neither binds nothing and no internal
	 * listener pool is created for it. */
	/* Every type with an operator endpoint registers its status route here, and
	 * the other half of the rule -- that the pool's own listener does not answer
	 * the same path -- is enforced in the child, by
	 * fpm_child_operator_endpoint_owns_status() in fpm_children.c for the types
	 * that go through fpm_main.c and by fpm_http_direct_ops_init_child() for
	 * http-direct. One directive, one page, one socket. */
	if (status_path && *status_path) {
		const char *listen = wp->config->pm_status_listen;

		if (!listen || !*listen) {
			listen = FPM_OPERATOR_ENDPOINT_DEFAULT_LISTEN;
		}
		if (0 > fpm_operator_endpoint_add_route(wp, "pm.status_path", listen, status_path,
				FPM_OPERATOR_FORMAT_JSON)) {
			return -1;
		}
	}

	if (metrics_path && *metrics_path) {
		const char *listen = wp->config->pm_metrics_listen;

		if (!listen || !*listen) {
			listen = FPM_OPERATOR_ENDPOINT_DEFAULT_LISTEN;
		}
		if (0 > fpm_operator_endpoint_add_route(wp, "pm.metrics_path", listen, metrics_path,
				FPM_OPERATOR_FORMAT_PROMETHEUS)) {
			return -1;
		}
	}

	return 0;
}
/* }}} */

static void fpm_operator_endpoint_dispatch(void *ctx, const char *path, const char *query,
	struct fpm_operator_reply_s *reply) /* {{{ */
{
	struct fpm_operator_listener_s *l = ctx;
	struct fpm_operator_route_s *r;

	for (r = l->routes; r; r = r->next) {
		const struct fpm_pool_type_s *type;

		if (strcmp(r->path, path) != 0) {
			continue;
		}

		if (r->format == FPM_OPERATOR_FORMAT_PROMETHEUS) {
			/* One exposition format for every pool, by design: a scraper reads
			 * one endpoint and gets labelled series it can compare across
			 * pools, which a per-type body would take away. */
			fpm_operator_page_render_prometheus(&reply->body, r->pool);
			reply->handled = 1;
			return;
		}

		/* The status page, on the other hand, is the type's own if the type has
		 * one: http-direct's carries per-connection counters and, on "?full", a
		 * row per child (issue #64), and issue #275 moved that page here rather
		 * than replacing it with the summary. Which types have one is data on
		 * the type (fpm_pool_type_s.operator_status), never a name test. */
		type = fpm_pool_type_of(r->pool);
		if (type->operator_status) {
			type->operator_status(r->pool, query, reply);
		} else {
			reply->content_type = "application/json";
			fpm_operator_page_render_json(&reply->body, r->pool);
		}
		reply->handled = 1;
		return;
	}
}
/* }}} */

void fpm_operator_endpoint_child_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct fpm_operator_listener_s *l;

	for (l = fpm_operator_listeners; l; l = l->next) {
		if (l->wp == wp) {
			fpm_operator_http_serve(wp->listening_socket, fpm_operator_endpoint_dispatch, l,
				l->known_paths);
			return;		/* not reached; _serve() does not return */
		}
	}

	/* A listener pool exists only because this file created it and recorded it,
	 * and the table is inherited across fork(), so this is unreachable rather
	 * than an error an operator can cause. Say so and exit instead of serving
	 * an endpoint that would answer nothing. */
	zlog(ZLOG_ERROR, "[pool %s] no operator route table for this listener; not serving",
		wp->config->name);
	exit(FPM_EXIT_SOFTWARE);
}
/* }}} */
