/* fpm-ng: the gateway's child processes: spawn, respawn with a burst limit, reload and cleanup.
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

static void fpm_http_read_deadline_forget(struct fpm_http_read_deadline_s *dl);
static void fpm_http_read_deadline_eof(evutil_socket_t fd, short what, void *arg);
static void fpm_http_read_deadline_arm_eof(evutil_socket_t fd, short what, void *arg);

/* A crash loop (bad bind, OOM, ...) must not turn into an unbounded fork()
 * storm: after this many respawns within RESPAWN_WINDOW seconds, a gateway
 * slot gives up and stays dead until the next reload. */
#define FPM_HTTP_RESPAWN_MAX_BURST 5
#define FPM_HTTP_RESPAWN_WINDOW_SEC 10

/* ---------------------------------------------------------------- processes */

/* Turns one target's configured listen address into the sockaddr its
 * connections are opened against. Per target since issue #340; for a gateway
 * with no http.route[] the one target is the pool's own listener and this is
 * the call it always made. */
static int fpm_http_resolve_upstream(struct fpm_http_target_s *t)
{
	char *address = t->listen_address;

	if (fpm_sockets_domain_from_address(address) == FPM_AF_UNIX) {
		struct sockaddr_un *sa_un = (struct sockaddr_un *) &t->upstream_addr;

		sa_un->sun_family = AF_UNIX;
		strlcpy(sa_un->sun_path, address, sizeof(sa_un->sun_path));
		t->upstream_len = sizeof(*sa_un);
		return 0;
	} else {
		struct addrinfo hints, *res;
		char *dup_address = strdup(address), *host = NULL, *port = strrchr(dup_address, ':');
		int ret;

		if (port) {
			*port++ = '\0';
			host = dup_address;
			if (host[0] == '[' && host[strlen(host) - 1] == ']') {
				host[strlen(host) - 1] = '\0';
				host++;
			}
		} else {
			port = dup_address; /* a bare port listens on any address */
		}
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		ret = getaddrinfo(host ? host : "localhost", port, &hints, &res);
		if (ret == 0) {
			memcpy(&t->upstream_addr, res->ai_addr, res->ai_addrlen);
			t->upstream_len = res->ai_addrlen;
			freeaddrinfo(res);
		}
		free(dup_address);
		return ret == 0 ? 0 : -1;
	}
}

/* Drops the gateway process from the master's identity (root, in the usual
 * deployment where the master binds privileged ports) to the pool's own
 * 'user'/'group' -- the same identity fpm_unix_init_child() (fpm_unix.c) puts
 * the request workers under. Called once per gateway process, after the last
 * operation that can need root (see the call site in fpm_http_gateway_run())
 * and before the event loop ever accepts a connection.
 *
 * A pool with no 'user'/'group' at all is only possible under FPM's explicit
 * run_as_root escape hatch (fpm_unix_conf_wp() refuses it otherwise) -- the
 * operator asked for root there, so the gateway stays root too, same as a
 * worker would. Anything else -- setgid/initgroups/setuid actually failing --
 * is fatal: never continue serving TLS with the private key as root. */
static void fpm_http_gateway_drop_privileges(struct fpm_http_gateway_s *gw) /* {{{ */
{
	if (geteuid() != 0) {
		return; /* the master was not root either, nothing to drop */
	}

	if (!gw->drop_uid && !gw->drop_gid) {
		zlog(ZLOG_WARNING, "[pool %s] http gateway: pool has no user/group, gateway keeps running as root", gw->pool);
		return;
	}

	if (setgid(gw->drop_gid) != 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] http gateway: failed to setgid(%d)", gw->pool, (int) gw->drop_gid);
		exit(FPM_EXIT_SOFTWARE);
	}
	if (initgroups(gw->drop_user, gw->drop_gid) != 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] http gateway: failed to initgroups(%s, %d)", gw->pool, gw->drop_user, (int) gw->drop_gid);
		exit(FPM_EXIT_SOFTWARE);
	}
	if (setuid(gw->drop_uid) != 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] http gateway: failed to setuid(%d)", gw->pool, (int) gw->drop_uid);
		exit(FPM_EXIT_SOFTWARE);
	}
	if (geteuid() == 0) {
		/* setuid(0) target, or a libc/capability quirk that made it a no-op:
		 * either way, never serve a TLS private key as root */
		zlog(ZLOG_ERROR, "[pool %s] http gateway: still root after dropping privileges", gw->pool);
		exit(FPM_EXIT_SOFTWARE);
	}
}
/* }}} */

/* The read deadline fired while the client was still delivering its first
 * request. The connection must NOT be torn down by hand: the bufferevent is
 * owned by evhttp's evhttp_connection, and bufferevent_free() underneath it
 * is a use-after-free (this exact mistake SIGSEGVed every gateway under the
 * tls-reload load loop on the test box). Instead, shrink the connection's
 * own read timeout to (almost) zero -- bufferevent_set_timeouts() re-arms
 * the pending read event through be_ops->adj_timeouts, so the timeout fires
 * immediately and evhttp closes the connection itself, through its own
 * error path, with the connection state consistent. A connection whose read
 * is not currently armed keeps existing, which is benign: it is either idle
 * keep-alive (harmless) or about to arm read again.
 *
 * dl->bev is valid here because arm() holds a reference of its own, NOT
 * because the connection is still one evhttp owns -- it may well not be. The
 * comment that used to stand here claimed the opposite ("a fired deadline
 * always refers to a connection evhttp still owns", on the grounds that the
 * EOF watcher below would have disarmed the node otherwise) and that claim
 * was measurably false: when evhttp frees the bufferevent it closes the fd,
 * epoll drops the watcher's registration without telling libevent, the
 * watcher never fires again, and this call reached into freed memory --
 * SIGSEGV inside bufferevent_set_timeouts(), roughly http.read_timeout after
 * a burst of aborted TLS connections (issue #90, backtrace on the poligon
 * 2026-09-08). With the reference held, a deadline that fires on a
 * connection evhttp has already dropped merely re-arms a timeout nobody is
 * listening to, and the decref in forget() then closes the socket. */
static void fpm_http_read_deadline_fire(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_http_read_deadline_s *dl = arg;
	static const struct timeval now = { 0, 1 };

	(void) fd;
	(void) what;
	bufferevent_set_timeouts(dl->bev, &now, &now);
	fpm_http_read_deadline_forget(dl);
}
/* The peer closed the connection before its first request completed
 * (EV_EOF), or libevent reports the fd as dead. Since issue #90 nothing here
 * is load-bearing for safety -- the reference taken in arm() is -- but the
 * connection is over, so forgetting the node now releases that reference,
 * and with it the fd, instead of holding both for whatever is left of the
 * deadline. The watcher also sees EV_READ whenever a trickle byte arrives;
 * only EOF (a zero-length peek) disarms. */
static void fpm_http_read_deadline_eof(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_http_read_deadline_s *dl = arg;
	bufferevent_data_cb readcb = NULL;
	char c;
	ssize_t n;

	(void) fd;
	if (what & EV_READ) {
		/* bufferevent_free() clears the callbacks (libevent-2.1.12
		 * bufferevent.c:809) and our reference keeps the object and its fd
		 * alive past that point, so "no read callback" means evhttp is done
		 * with this connection and nobody will consume what is still in the
		 * socket buffer. Checked BEFORE the peek: unread bytes look exactly
		 * like a live peer to it, and on a level-triggered EV_READ that
		 * would spin the event loop for the rest of the deadline. */
		bufferevent_getcb(dl->bev, &readcb, NULL, NULL, NULL);
		if (readcb) {
			n = recv(dl->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
			/* fpm_http_would_block(), not "EAGAIN || EWOULDBLOCK": the two
			 * are the same value on Linux, which gcc reports as
			 * -Wlogical-op, and that helper already carries the
			 * #if EWOULDBLOCK != EAGAIN dance for the systems where they
			 * differ. */
			if (n > 0 || (n < 0 && fpm_http_would_block(errno))) {
				return; /* data available or transient: still alive */
			}
		}
	}
	fpm_http_read_deadline_forget(dl);
}

/* Unlinks and frees a deadline node. Safe to call twice is NOT required --
 * both callers (fire and eof) free exactly once, and disarm() removes the
 * node from the list first, so neither callback can find it afterwards. */
static void fpm_http_read_deadline_forget(struct fpm_http_read_deadline_s *dl)
{
	struct fpm_http_read_deadline_s **p;

	for (p = &dl->gw->deadlines; *p; p = &(*p)->next) {
		if (*p == dl) {
			*p = dl->next;
			break;
		}
	}
	if (dl->ev) {
		event_free(dl->ev);
	}
	if (dl->ev_eof) {
		event_free(dl->ev_eof);
	}
	/* The reference arm() took. Last, and after both events are gone: when
	 * evhttp has already let go of this connection, this is the call that
	 * frees the bufferevent and closes its fd (BEV_OPT_CLOSE_ON_FREE), and
	 * the EOF watcher must not be registered on that fd when it goes. */
	bufferevent_decref(dl->bev);
	free(dl);
}

/* Arms the per-connection read deadline (see struct fpm_http_read_deadline_s).
 * The fd is read from the bev; on a TLS connection it is not yet assigned at
 * bevcb time (bufferevent_setfd happens right after, in evhttp), so the EOF
 * watcher is armed lazily on the first event loop pass via a zero timer. */
static void fpm_http_read_deadline_arm(struct fpm_http_gateway_s *gw, struct bufferevent *bev)
{
	struct fpm_http_read_deadline_s *dl = calloc(1, sizeof(*dl));

	if (!dl) {
		return; /* OOM: this connection gets no read deadline (libevent has no implicit one, issue #593); the listener still works */
	}
	dl->gw = gw;
	dl->bev = bev;
	dl->fd = -1;
	/* A reference of our own, released in forget(). evhttp frees this
	 * bufferevent as soon as the connection ends, which is routinely BEFORE
	 * the deadline fires; with a reference outstanding, bufferevent_free()
	 * only clears the callbacks and cancels pending operations
	 * (libevent-2.1.12 bufferevent.c:805-812), leaving the object, its
	 * events and its fd valid until the last reference goes. Every dl->bev
	 * and dl->fd use below rests on that, and nothing else -- see
	 * fpm_http_read_deadline_fire() for what happened without it (issue
	 * #90). Cost: a connection that dies before its first request keeps its
	 * fd until the deadline expires, unless the EOF watcher below gets to it
	 * first. */
	bufferevent_incref(bev);
	dl->ev = event_new(gw->base, -1, EV_TIMEOUT, fpm_http_read_deadline_fire, dl);
	dl->ev_eof = event_new(gw->base, -1, EV_TIMEOUT, fpm_http_read_deadline_arm_eof, dl);
	if (!dl->ev || !dl->ev_eof) {
		fpm_http_read_deadline_forget(dl);
		return;
	}
	dl->next = gw->deadlines;
	gw->deadlines = dl;
	event_add(dl->ev, &gw->read_timeout);
	{
		static const struct timeval zero = { 0, 0 };
		event_add(dl->ev_eof, &zero); /* re-armed as EV_READ once the fd is known */
	}
}

/* Second pass of arming: evhttp has called bufferevent_setfd() by now, so
 * dl->fd is knowable. Turns ev_eof into the persistent EOF watcher. The
 * bufferevent may already be gone from evhttp's point of view when this runs
 * -- a connection that fails in the same loop iteration it was accepted in
 * gets there first -- so reading its fd is safe only because of the
 * reference arm() holds. */
static void fpm_http_read_deadline_arm_eof(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_http_read_deadline_s *dl = arg;

	(void) fd;
	(void) what;
	dl->fd = bufferevent_getfd(dl->bev);
	if (dl->fd < 0) {
		/* still no fd (should not happen): without the watcher a peer close
		 * would leave a dangling bev, so drop the deadline entirely */
		fpm_http_read_deadline_forget(dl);
		return;
	}
	event_assign(dl->ev_eof, dl->gw->base, dl->fd, EV_READ | EV_PERSIST, fpm_http_read_deadline_eof, dl);
	event_add(dl->ev_eof, NULL);
}

/* The first request on this connection has fully arrived: its deadline is
 * spent. Safe to call when none is armed (read_timeout = 0 or OOM above). */
void fpm_http_read_deadline_disarm(struct fpm_http_gateway_s *gw, struct bufferevent *bev)
{
	struct fpm_http_read_deadline_s **p;

	if (!bev) {
		return;
	}
	for (p = &gw->deadlines; *p; p = &(*p)->next) {
		if ((*p)->bev == bev) {
			fpm_http_read_deadline_forget(*p);
			return;
		}
	}
}

/* The gateway's bevcb: builds the bufferevent for a new client connection and
 * arms its read deadline. TLS connections get their SSL bufferevent from
 * fpm_tls_http_bevcb() with the pool's CURRENT SSL_CTX (gw->tls_ctx), so a
 * hot-reloaded certificate (fpm_tls_reload.c) applies to new connections
 * without this wrapper being re-registered. */
static struct bufferevent *fpm_http_bevcb(struct event_base *base, void *arg)
{
	struct fpm_http_gateway_s *gw = arg;
	struct bufferevent *bev;

#ifdef HAVE_FPM_HTTP_TLS
	if (gw->tls_ctx) {
		bev = fpm_tls_http_bevcb(base, gw->tls_ctx);
	} else
#endif
	{
		bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);
	}
	if (bev && gw->read_timeout_ms > 0) {
		fpm_http_read_deadline_arm(gw, bev);
	}
	return bev;
}

/* The bevcb of the http.plain_listen listener: always a plain bufferevent,
 * never the TLS wrapper fpm_http_bevcb() applies when gw->tls_ctx is set --
 * this is the cleartext port. It arms the same first-request read deadline;
 * without one a client could connect and send nothing, forever (issue #593). */
static struct bufferevent *fpm_http_plain_bevcb(struct event_base *base, void *arg)
{
	struct fpm_http_gateway_s *gw = arg;
	struct bufferevent *bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);

	if (bev && gw->read_timeout_ms > 0) {
		fpm_http_read_deadline_arm(gw, bev);
	}
	return bev;
}

/* The gateway end of the log follow channel: readable means the master has
 * reopened its logs (SIGUSR1). Two things follow from that and they are
 * independent of each other:
 *
 * - the error_log descriptor the master sent has to be adopted, because this
 *   process cannot open that file itself (issue #134), and
 * - this process's own http.access_log has to be reopened by path, because
 *   this process is the only one that has it open at all (issue #137).
 *
 * Both live here, in the gateway, rather than in either module: this is where
 * the wakeup lands, and neither fpm_error_log_follow.c nor
 * fpm_http_access_log.c has to learn about the other. */
static void fpm_http_log_follow_readable(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_http_gateway_s *gw = arg;

	(void) fd;
	(void) what;

	if (fpm_error_log_follow_child_adopt() > 0) {
		gw->access_log = fpm_http_access_log_reopen(gw->access_log, gw->pool, gw->access_log_path);
	}
}

/* Gateway process, once it has its own event_base. The event is never freed:
 * this process only ever leaves through exit(), the same lifetime gw->base
 * itself has. */
static void fpm_http_log_follow_init(struct fpm_http_gateway_s *gw)
{
	int fd = fpm_error_log_follow_child_fd();
	struct event *ev;

	if (fd < 0) {
		/* Only when the master could not create the channel — it creates one
		 * for every gateway now, error_log = syslog included, because the
		 * notification is also this process's http.access_log wakeup
		 * (fpm_error_log_follow.h). The master has already logged why. */
		return;
	}

	ev = event_new(gw->base, fd, EV_READ | EV_PERSIST, fpm_http_log_follow_readable, gw);
	if (!ev || event_add(ev, NULL) != 0) {
		zlog(ZLOG_WARNING, "[pool %s] http: cannot watch the log follow channel; this process "
						   "will keep writing into the pre-rotation error_log and http.access_log",
				gw->pool);
	}
}

/* Opens this gateway process's TLS listener: puts the bound socket into
 * LISTEN and hands it to evhttp. Called either at child startup (the ordinary
 * case, and every case without http.tls_wait_for_cert) or from the
 * generation-watch timer at the NO_CERT -> READY transition (issue #172).
 * Returns 0 on success, -1 with the reason logged otherwise.
 *
 * listen() unconditionally, including on the startup path where
 * fpm_http_listen() already called it: listen() on a socket that is already
 * listening succeeds and only updates the backlog, so the one call covers
 * both entry points without either having to know which one it is.
 *
 * The return value of listen() is checked because it can genuinely fail here.
 * Measured on the test box, 2026-09-11: while our socket was bound but not
 * listening, another process bound AND listened on the same port, and our
 * later listen() then failed with EADDRINUSE. Binding early does not reserve
 * the port -- SO_REUSEADDR permits a second bind as long as nobody is in
 * LISTEN -- so a NO_CERT pool has a window in which its port can be taken.
 * Reporting that is issue #172 criterion 6: the alternative is a pool that
 * stays dark with nothing in the log to say why. */
static int fpm_http_gateway_open_tls_listener(struct fpm_http_gateway_s *gw) /* {{{ */
{
	struct evhttp_bound_socket *bound;

	if (listen(gw->listen_fd, gw->backlog) != 0) {
		zlog(ZLOG_ERROR, "[pool %s] http: cannot open the TLS listener: listen() failed: %s -- the certificate is installed but this gateway is not serving it",
				gw->pool, strerror(errno));
		return -1;
	}
	bound = evhttp_accept_socket_with_handle(gw->http, gw->listen_fd);
	if (!bound) {
		zlog(ZLOG_ERROR, "[pool %s] http: evhttp_accept_socket() failed", gw->pool);
		return -1;
	}
	if (fpm_http_accept_backoff_install(gw->base, bound, gw->pool, "main") != 0) {
		zlog(ZLOG_WARNING, "[pool %s] http: no accept backoff on the main listener; running out of file descriptors will make it spin", gw->pool);
	}
	gw->tls_ready = 1;
	return 0;
}
/* }}} */

#ifdef HAVE_FPM_HTTP_TLS
/* The above, as the void(void *) the generation-watch timer calls. A failure
 * is logged and this gateway stays in NO_CERT rather than exiting: exiting
 * would spend one of the crash-loop budget's five restarts per
 * FPM_HTTP_RESPAWN_WINDOW_SEC on a condition a restart cannot fix (the port
 * is held by someone else), and a gateway still answering HTTP-01 challenges
 * on http.plain_listen is strictly more useful than one that is gone. The
 * hook has already been cleared by the time this runs, so there is no retry:
 * the log line is the whole signal. */
static void fpm_http_gateway_tls_listener_hook(void *arg) /* {{{ */
{
	(void) fpm_http_gateway_open_tls_listener((struct fpm_http_gateway_s *) arg);
}
/* }}} */
#endif

static void fpm_http_gateway_run(struct fpm_http_gateway_s *gw, unsigned index) /* {{{ */
{
	struct fpm_worker_pool_s *wp;
	struct sigaction act;
	char title[128];
	unsigned i;

	fpm_globals.is_child = 1;

	/* ... which is what stops zlog() from timestamping this process's lines,
	 * and this process — unlike an upstream FPM child — writes them into the
	 * master's error_log itself (issue #130, fpm_child_error_log.h). */
	fpm_child_error_log_use();
	/* ... and, because it writes them itself, it is also the process that has
	 * to be told when the master reopens that file — issue #134, and the same
	 * notification is what tells it to reopen its own http.access_log, issue
	 * #137. Keeps this slot's channel and drops the ones belonging to gateways
	 * forked before it. */
	fpm_error_log_follow_child(gw->slots[index]->log_follow);

	/* plain defaults: the master terminates us with a signal, nothing to clean up */
	memset(&act, 0, sizeof(act));
	act.sa_handler = SIG_DFL;
	sigaction(SIGTERM, &act, 0);
	sigaction(SIGINT, &act, 0);
	sigaction(SIGQUIT, &act, 0);
	sigaction(SIGUSR1, &act, 0);
	sigaction(SIGUSR2, &act, 0);
	sigaction(SIGCHLD, &act, 0);
	act.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &act, 0);
	fpm_signals_unblock();

	/* The pools' FastCGI listeners are the master's business. Skip a pool with
	 * no listener at all (requires_listen = 0: cron, supervisor) and a
	 * proxy_only inet gateway, whose listening_socket the master already set to
	 * -1 because it never created one -- closing the underlying fd 0 would be
	 * closing this process's stdin. Issue #388. */
	for (wp = fpm_worker_all_pools; wp; wp = wp->next) {
		if (!fpm_pool_type_of(wp)->requires_listen || wp->listening_socket < 0) {
			continue;
		}
		close(wp->listening_socket);
	}

	snprintf(title, sizeof(title), "http gateway %s [%u]", gw->pool, index);
	fpm_env_setproctitle(title);

	/* Issue #390: claim this process's own block of the pool's counters
	 * segment, indexed by the slot the master spawned it as. Only this process
	 * ever writes these cells, so no atomic is needed; zeroing them here (they
	 * should already be zero -- the master zeroes a dead process's block -- but
	 * a fresh process must never inherit a stale gauge) is the child's half of
	 * the #333 pattern. */
	if (gw->counters && index < gw->counters->nproc) {
		unsigned g;

		gw->gauges = fpm_http_counters_gauges(gw->counters, index);
		for (g = 0; g < 1u + gw->counters->nslots; g++) {
			gw->gauges[g] = 0;
		}
	}

	/* Where this process starts on the issue #172 state machine. Provisional
	 * on purpose, and used below for one thing only: whether the reuseport
	 * bind should listen() immediately. The authoritative value is assigned
	 * after the SSL_CTX is built, from the context itself; the late listen()
	 * in fpm_http_gateway_open_tls_listener() is idempotent, so a socket that
	 * this leaves unlistened costs nothing but the call. */
	gw->tls_ready = !gw->tls_wait_for_cert;

	if (gw->reuseport) {
		/* own listening socket in the SO_REUSEPORT group, the kernel spreads connections by hash;
		 * the last thing that can need root, so the privilege drop below waits for it */
		close(gw->listen_fd);
		/* Issue #388: on the gateway, `listen` itself is the HTTP address, so
		 * binding it must not bump the port by one the way an http pool's
		 * FastCGI listen does. Passing the same address as http_address is
		 * what says "use this port exactly". */
		gw->listen_fd = fpm_http_listen(gw->pool, gw->listen_address,
				gw->proxy_only ? gw->listen_address : gw->http_listen_override,
				gw->backlog, 1, gw->tls_ready);
		if (gw->listen_fd < 0) {
			exit(FPM_EXIT_SOFTWARE);
		}
		if (gw->plain_listen_address) {
			close(gw->plain_listen_fd);
			gw->plain_listen_fd = fpm_http_listen(gw->pool, gw->listen_address, gw->plain_listen_address, gw->backlog, 1, 1);
			if (gw->plain_listen_fd < 0) {
				exit(FPM_EXIT_SOFTWARE);
			}
		}
	}

	/* Everything above this line is the only reason the gateway ever needed
	 * root: binding http.reuseport's own listener, and holding the TLS
	 * private key the master read before the first fork (fpm_tls_http.h).
	 * Nothing below -- the access log, static files, TLS handshakes, proxying
	 * to the pool -- needs it. See task 010 (done; see docs/task-archive.md). */
	fpm_http_gateway_drop_privileges(gw);

	/* one fd per gateway process, all appending to the same http.access_log
	 * path -- see fpm_http_access_log.h for why that does not interleave.
	 * Opened after the drop so the file is created by the dropped-to identity,
	 * which is also what lets this process reopen it after a logrotate on its
	 * own (issue #137, fpm_http_log_follow_readable()). */
	gw->access_log = fpm_http_access_log_open(gw->pool, gw->access_log_path);

	/* One resolve per target, in this gateway process, exactly where the one
	 * resolve used to be (issue #340). A name that no longer resolves is still
	 * fatal for the process, not for the one target: a gateway that came up
	 * with a target it can never reach would answer 502 for that prefix
	 * forever with nothing in the log after startup to say why. */
	for (i = 0; i < gw->ntargets; i++) {
		struct fpm_http_target_s *t = &gw->targets[i];

		if (fpm_http_resolve_upstream(t) != 0) {
			if (strcmp(t->pool, gw->pool) == 0) {
				zlog(ZLOG_ERROR, "[pool %s] http: cannot resolve '%s'", gw->pool, t->listen_address);
			} else {
				zlog(ZLOG_ERROR, "[pool %s] http: cannot resolve '%s' for http.route target pool '%s'",
						gw->pool, t->listen_address, t->pool);
			}
			exit(FPM_EXIT_SOFTWARE);
		}
		TAILQ_INIT(&t->upstreams);
		TAILQ_INIT(&t->waiting);
	}

	/* Issue #389: the same one resolve per operator listener this gateway
	 * forwards to. Separate from the targets above because an operator target
	 * is not a routed pool and must not appear in the #341 metrics page. */
	for (i = 0; i < gw->noperator_targets; i++) {
		struct fpm_http_target_s *t = &gw->operator_targets[i];

		if (fpm_http_resolve_upstream(t) != 0) {
			zlog(ZLOG_ERROR, "[pool %s] http.operator: cannot resolve the operator listener '%s'",
					gw->pool, t->listen_address);
			exit(FPM_EXIT_SOFTWARE);
		}
		TAILQ_INIT(&t->upstreams);
		TAILQ_INIT(&t->waiting);
	}

	gw->base = event_base_new();
	gw->http = evhttp_new(gw->base);
	fpm_http_log_follow_init(gw);
#ifdef HAVE_FPM_HTTP_TLS
	if (gw->tls || gw->tls_wait_for_cert) {
		/* Own SSL_CTX per gateway process, built from cert/key bytes the
		 * master already read and validated, never from an SSL_CTX inherited
		 * through fork() -- see fpm_tls_http.h.
		 *
		 * The bytes come from the reload machinery's currently published slot
		 * whenever there is one, not from gw->tls: gw->tls is what the master
		 * read once before the FIRST fork, so a gateway respawned after N
		 * certificate reloads would otherwise start with -- and, believing it
		 * was up to date, keep -- the startup certificate (issue #91). This is
		 * the "builds its SSL_CTX from the currently published slot" branch of
		 * that issue: the respawned process is correct from its first accepted
		 * connection, with no adoption tick and no window in between. For a
		 * gateway forked at startup the published slot is generation 0, i.e.
		 * exactly gw->tls's bytes, so nothing about startup changes. */
		gw->tls_ctx = fpm_tls_reload_child_ctx_new(gw->reload);
		if (!gw->tls_ctx && gw->tls) {
			gw->tls_ctx = fpm_tls_http_ctx_new(gw->pool, gw->tls);
		}
		if (!gw->tls_ctx && !gw->tls_wait_for_cert) {
			exit(FPM_EXIT_SOFTWARE);
		}
		/* THE decision, taken once, from the context this process actually
		 * holds. An earlier version sampled fpm_tls_reload_has_cert()
		 * before the fork-time work above and treated that as the state; the
		 * master can publish a generation in between, and then a gateway
		 * respawned near the transition (issue #91) could reach
		 * fpm_http_gateway_open_tls_listener() with gw->tls_ctx == NULL --
		 * fpm_http_bevcb() has no context to wrap the connection in and falls
		 * through to a plain bufferevent, i.e. cleartext HTTP served on the
		 * TLS port. The mirror case lost the transition instead: the timer had
		 * already latched the generation, so the hook never fired and that
		 * gateway stayed dark for good.
		 *
		 * Deriving both the flag and the hook from gw->tls_ctx makes all three
		 * states impossible: a context means listening, no context means
		 * NO_CERT plus exactly one armed hook. */
		if (gw->tls_wait_for_cert) {
			gw->tls_ready = gw->tls_ctx != NULL;
		}
		if (!gw->tls_ready) {
			/* NO_CERT (issue #172): no context, and therefore nothing to
			 * accept a TLS connection with. The hook below is what turns
			 * this process READY, and it runs on this child's own
			 * generation-watch timer, so every gateway makes the transition
			 * independently -- criterion 4. */
			fpm_tls_reload_child_on_first_cert(gw->reload, fpm_http_gateway_tls_listener_hook, gw);
		}

		/* Own generation-watch timer, on this child's own base -- see
		 * fpm_tls_reload.h. No-op when gw->reload is NULL. The bevcb
		 * pair keeps a reload from dropping the read deadline (task 031). */
		fpm_tls_reload_child_init(gw->reload, gw->base, gw->http, &gw->tls_ctx,
				fpm_http_bevcb, gw);
	}
#endif
	/* One bevcb for every listener (TLS and plain alike): it wraps the
	 * bufferevent AND arms the per-connection read deadline (task 031).
	 * evhttp's own timeout is NOT used -- a bufferevent read timeout is an
	 * idle timer restarted on every received byte, so a slow-loris client
	 * trickling one byte at a time would never trip it. See
	 * struct fpm_http_read_deadline_s. */
	evhttp_set_bevcb(gw->http, fpm_http_bevcb, gw);
	evhttp_set_allowed_methods(gw->http, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD | EVHTTP_REQ_PUT |
												 EVHTTP_REQ_DELETE | EVHTTP_REQ_OPTIONS | EVHTTP_REQ_PATCH);
	evhttp_set_max_body_size(gw->http, gw->max_body);
	/* Without this the block limit is libevent's default EV_SIZE_MAX (libevent
	 * 2.1.12-stable, http.c:3678 in evhttp_new_object()): a client could send
	 * headers until the process died, and unlike a direct-transport worker
	 * this one process serves every connection of the pool, so that memory is
	 * charged against every in-flight request here. #115 bounded a header
	 * *name*, which does nothing about their number. The refusal is
	 * libevent's, not ours: over the limit it fails the connection with
	 * EVREQ_HTTP_INVALID_HEADER (http.c:2303 evhttp_read_header()), which for
	 * an incoming connection answers 400 and closes (http.c:664
	 * evhttp_connection_incoming_fail()) -- the same status our own
	 * over-long-name check returns. The request line is charged against the
	 * same budget (http.c:2041), so a pathological URI is bounded too.
	 * Issue #117. */
	evhttp_set_max_headers_size(gw->http, FPM_HTTP_HEADERS_MAX);
	evhttp_set_gencb(gw->http, fpm_http_request, gw);
	evutil_make_socket_nonblocking(gw->listen_fd);
	/* In NO_CERT the fd is bound but was never listen()ed, and
	 * evhttp_accept_socket() would not fix that: it reaches
	 * evconnlistener_new() with a backlog of 0, which skips listen()
	 * entirely (libevent 2.1.12-stable, listener.c). Accepting here would
	 * therefore silently produce a listener that never fires. The pair of
	 * calls belongs together, and it belongs in one place --
	 * fpm_http_gateway_open_tls_listener(), which the transition also uses. */
	if (gw->tls_ready && fpm_http_gateway_open_tls_listener(gw) != 0) {
		exit(FPM_EXIT_SOFTWARE);
	}
	if (gw->plain_listen_fd >= 0) {
		struct evhttp *plain = evhttp_new(gw->base);
		struct evhttp_bound_socket *plain_bound;

		if (!plain) {
			exit(FPM_EXIT_SOFTWARE);
		}
		evhttp_set_allowed_methods(plain, EVHTTP_REQ_GET | EVHTTP_REQ_HEAD);
		evhttp_set_max_body_size(plain, 0);
		/* Same bound on http.plain_listen: it is the same process and the same
		 * unauthenticated listener, and it answers before TLS, so leaving it
		 * at EV_SIZE_MAX would leave the hole open on the easier port. */
		evhttp_set_max_headers_size(plain, FPM_HTTP_HEADERS_MAX);
		evhttp_set_bevcb(plain, fpm_http_plain_bevcb, gw);
		evhttp_set_gencb(plain, fpm_http_plain_request, gw);
		evutil_make_socket_nonblocking(gw->plain_listen_fd);
		plain_bound = evhttp_accept_socket_with_handle(plain, gw->plain_listen_fd);
		if (!plain_bound) {
			zlog(ZLOG_ERROR, "[pool %s] http: evhttp_accept_socket() failed for http.plain_listen", gw->pool);
			exit(FPM_EXIT_SOFTWARE);
		}
		if (fpm_http_accept_backoff_install(gw->base, plain_bound, gw->pool, "http.plain_listen") != 0) {
			zlog(ZLOG_WARNING, "[pool %s] http: no accept backoff on http.plain_listen; running out of file descriptors will make it spin", gw->pool);
		}
	}

	event_base_dispatch(gw->base);
	exit(FPM_EXIT_OK);
}
/* }}} */

/* Listens on http_address when given, otherwise on the FastCGI address with the port bumped by
 * one. Returns -1 when that is not possible.
 *
 * do_listen = 0 binds the socket and stops there, for http.tls_wait_for_cert's
 * NO_CERT state (issue #172). Measured on the test box, 2026-09-11: a socket
 * bound but never listen()ed answers a connect with RST -- curl reports
 * "Failed to connect ... Could not connect to server" -- and does not appear
 * in `ss -lnt`. That is exactly the "connection refused, not a handshake
 * failure and not a plain-HTTP answer" criterion 3 asks for.
 *
 * Why bind here rather than defer the whole socket to the transition: every
 * gateway accepts on ONE inherited fd, so the fd has to exist before the
 * first fork. Deferring the bind would leave each child binding its own, which
 * fails with EADDRINUSE unless http.reuseport is on.
 *
 * What binding early does NOT buy, measured in the same run and contrary to
 * what this code was first written assuming: it does not reserve the port. A
 * second process with SO_REUSEADDR bound and listened on the same port while
 * ours was bound-but-not-listening. Hence fpm_http_gateway_open_tls_listener()
 * checks its late listen() and reports EADDRINUSE rather than assuming it. */
int fpm_http_listen(const char *pool, const char *listen_address, const char *http_address, int backlog, int reuseport, int do_listen) /* {{{ */
{
	char *dup_address = strdup(http_address ? http_address : listen_address), *host = NULL, *port_str = strrchr(dup_address, ':');
	char port[sizeof("65535")];
	struct addrinfo hints, *res, *p;
	int fd = -1, port_no, on = 1;

	if (port_str) {
		*port_str++ = '\0';
		host = dup_address;
		if (host[0] == '[' && host[strlen(host) - 1] == ']') {
			host[strlen(host) - 1] = '\0';
			host++;
		}
	} else {
		port_str = dup_address;
	}
	port_no = atoi(port_str) + (http_address ? 0 : 1);
	if (port_no < 1 || port_no > 65535) {
		zlog(ZLOG_WARNING, "[pool %s] no HTTP listener: no port left above '%s'", pool, listen_address);
		free(dup_address);
		return -1;
	}
	snprintf(port, sizeof(port), "%d", port_no);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if (getaddrinfo(host, port, &hints, &res) != 0) {
		zlog(ZLOG_WARNING, "[pool %s] no HTTP listener: cannot resolve '%s'", pool, listen_address);
		free(dup_address);
		return -1;
	}
	for (p = res; p && fd < 0; p = p->ai_next) {
		fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (fd < 0) {
			continue;
		}
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
#ifdef SO_REUSEPORT
		if (reuseport) {
			setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
		}
#endif
		if (bind(fd, p->ai_addr, p->ai_addrlen) != 0 || (do_listen && listen(fd, backlog) != 0)) {
			zlog(ZLOG_WARNING, "[pool %s] no HTTP listener: unable to listen on %s:%s: %s", pool, host ? host : "*", port, strerror(errno));
			close(fd);
			fd = -1;
			break;
		}
		fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
	}
	freeaddrinfo(res);
	free(dup_address);
	return fd;
}
/* }}} */

static void fpm_http_gateway_on_exit(void *arg, pid_t old_pid, int status);

void fpm_http_gateway_spawn(struct fpm_http_gateway_s *gw, unsigned index) /* {{{ */
{
	struct fpm_http_gw_slot_s *slot = gw->slots[index];

	/* Before the fork, so the child inherits its receiving end (issue #134).
	 * A slot that already had one is a slot whose process is gone. */
	fpm_error_log_follow_free(slot->log_follow);
	slot->log_follow = fpm_error_log_follow_new();

	gw->pids[index] = fork();
	if (gw->pids[index] < 0) {
		zlog(ZLOG_SYSERROR, "[pool %s] http: fork() failed", gw->pool);
		fpm_error_log_follow_free(slot->log_follow);
		slot->log_follow = NULL;
	} else if (gw->pids[index] == 0) {
		fpm_http_gateway_run(gw, index);
		/* not reached */
	} else {
		fpm_error_log_follow_parent(slot->log_follow);
		fpm_children_extra_watch(gw->pids[index], fpm_http_gateway_on_exit, slot);
	}
}
/* }}} */

/* Called by fpm_children_bury() (via fpm_children_extra_handle_exit()) when a
 * gateway process dies, however it dies — crash, OOM kill, whatever. Never
 * called for a deliberate shutdown: fpm_http_cleanup() forgets the pid first.
 * "Master respawns it like any other child" (docs/NOTES.md) without teaching
 * fpm_children.c anything about gateways — see fpm_children_extra.h. */
static void fpm_http_gateway_on_exit(void *arg, pid_t old_pid, int status) /* {{{ */
{
	struct fpm_http_gw_slot_s *slot = arg;
	struct fpm_http_gateway_s *gw = slot->gw;
	time_t now = time(NULL);

	/* Issue #390 review: this process's sockets are closed now, but no close
	 * callback ran for them. Reconcile its per-process gauges and its share of
	 * the shared upstream budget before anything else -- including before the
	 * gave_up early return below, so the death that sets gave_up also releases
	 * what it held. */
	fpm_http_counters_process_gone(gw, slot->index);

	/* The process behind it is reaped; a slot that is respawned below gets a
	 * fresh channel in fpm_http_gateway_spawn(), and one that is not must not
	 * leave the master holding an end nobody reads (issue #134). */
	fpm_error_log_follow_free(slot->log_follow);
	slot->log_follow = NULL;

	if (slot->respawn.gave_up) {
		return; /* already logged once below, do not spam on every further death */
	}

	if (now - slot->respawn.window_start > FPM_HTTP_RESPAWN_WINDOW_SEC) {
		slot->respawn.window_start = now;
		slot->respawn.count = 0;
	}
	slot->respawn.count++;

	if (WIFSIGNALED(status)) {
		zlog(ZLOG_WARNING, "[pool %s] http gateway %u (pid %d) killed by signal %d, respawning",
				gw->pool, slot->index, (int) old_pid, WTERMSIG(status));
	} else {
		zlog(ZLOG_WARNING, "[pool %s] http gateway %u (pid %d) exited with code %d, respawning",
				gw->pool, slot->index, (int) old_pid, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	}

	if (!fpm_pctl_can_spawn_children()) {
		/* master is stopping/reloading: fpm_http_cleanup() is about to run
		 * (or already did) and will forget the survivors; nothing to spawn */
		gw->pids[slot->index] = 0;
		return;
	}

	if (slot->respawn.count > FPM_HTTP_RESPAWN_MAX_BURST) {
		slot->respawn.gave_up = 1;
		gw->pids[slot->index] = 0;
		zlog(ZLOG_ALERT, "[pool %s] http gateway %u crashed %u times within %d seconds, giving up on it "
						 "(reload to try again); the pool now has one fewer gateway",
				gw->pool, slot->index, slot->respawn.count, FPM_HTTP_RESPAWN_WINDOW_SEC);
		return;
	}

	fpm_http_gateway_spawn(gw, slot->index);
}
/* }}} */

void fpm_http_cleanup(int which, void *arg) /* {{{ */
{
	struct fpm_http_gateway_s *gw, *next;
	unsigned i;

	for (gw = gateways; gw; gw = next) {
		next = gw->next;
		for (i = 0; i < gw->nproc; i++) {
			if (gw->pids[i] > 0) {
				/* forget it BEFORE signalling it: this is a deliberate kill,
				 * not a crash, so fpm_children_extra_handle_exit() must not
				 * respawn it when the master's SIGCHLD handler reaps it */
				fpm_children_extra_forget(gw->pids[i]);
				kill(gw->pids[i], SIGTERM);
			}
		}
		for (i = 0; i < gw->nproc; i++) {
			if (gw->pids[i] > 0) {
				waitpid(gw->pids[i], NULL, 0);
			}
		}
		if (gw->listen_fd >= 0) {
			close(gw->listen_fd);
		}
		if (gw->plain_listen_fd >= 0) {
			close(gw->plain_listen_fd);
		}
		fpm_http_routes_free(gw);
		fpm_http_operator_free(gw); /* issue #389 */
#ifdef HAVE_FPM_HTTP_TLS
		if (gw->reload) {
			fpm_tls_reload_free(gw->reload);
		}
#endif
		for (i = 0; i < gw->nproc; i++) {
			fpm_error_log_follow_free(gw->slots[i]->log_follow);
			free(gw->slots[i]);
		}
		free(gw->slots);
		free(gw->pids);
		fpm_http_acl_free(gw->acl);
		free(gw->allowed_clients);
		fpm_http_acl_free(gw->trusted_proxies_acl);
		free(gw->trusted_proxies);
		free(gw->front_controller);
		free(gw->access_log_path);
		free(gw->http_listen_override);
		free(gw->plain_listen_address);
		free(gw->ping_path);
		free(gw->ping_response);
		{
			unsigned j;

			for (j = 0; j < gw->suppress_paths_count; j++) {
				free(gw->suppress_paths[j]);
			}
			free(gw->suppress_paths);
		}
		free(gw->pool);
		free(gw->listen_address);
		free(gw->docroot);
		free(gw);
	}
	gateways = NULL;
}
/* }}} */

#endif /* HAVE_FPM_HTTP */
