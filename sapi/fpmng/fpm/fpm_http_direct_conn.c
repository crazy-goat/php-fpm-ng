/* See fpm_http_direct_conn.h for what this file is for and why it is built
 * the way it is. */

#include "fpm_config.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#include <event2/event.h>
#include <event2/bufferevent.h>

#include "fpm_http_direct_conn.h"
#include "zlog.h"

/* One tracked connection. Created from the bevcb, released by whichever of the
 * three comes first: the EOF watcher, the deadline, or the sweep. */
struct fpm_direct_conn {
	struct fpm_http_direct_conns *conns;
	struct bufferevent *bev;	/* one reference of ours, held for the node's whole life */
	evutil_socket_t fd;		/* -1 until the pickup pass */
	struct event *deadline;		/* first-request deadline; freed when it is spent */
	struct event *watch;		/* first a zero timer (fd pickup), then the EOF watcher */
	struct sockaddr_storage peer;
	socklen_t peer_len;
	int checked;	/* the per-client cap has already passed judgement on this one */
	int served;			/* the first request arrived: only the limits keep this node */
	/* issue #62 (fpm_connection_info()): when this node was created (the
	 * bevcb, i.e. accept time) and how many requests have completed
	 * fpm_http_direct_conns_request() on it so far. */
	struct timeval accepted;
	unsigned requests;
	/* Doubly linked, with a tail, so that unlinking one node is O(1). It used
	 * to be a singly linked list walked from the head, which was fine while
	 * the list only held connections waiting for their first request; since
	 * issue #64 it holds every connection this child has, and a walk per close
	 * is a walk per connection. */
	struct fpm_direct_conn *next;
	struct fpm_direct_conn *prev;
};

struct fpm_http_direct_conns {
	struct event_base *base;
	struct fpm_http_direct_conns_limits limits;
	/* Most recently touched at the head, so the sweep can start at the tail:
	 * a connection that just made a request is the least likely to be the one
	 * that ended. */
	struct fpm_direct_conn *list;
	struct fpm_direct_conn *tail;
	unsigned live;
	unsigned long timed_out;
	unsigned long refused;
	int said_timed_out;	/* each limit announces itself once per child, see below */
	int said_refused;
};

/* The same helper fpm_http.c:467 has, duplicated rather than exported: two
 * lines of preprocessor are cheaper than a shared header for one inline, and
 * "EAGAIN || EWOULDBLOCK" written out is a -Wlogical-op warning on Linux. */
static inline int fpm_direct_conn_would_block(int err)
{
	if (err == EAGAIN) {
		return 1;
	}
#if EWOULDBLOCK != EAGAIN
	if (err == EWOULDBLOCK) {
		return 1;
	}
#endif
	return 0;
}

static int fpm_direct_conn_limited(const struct fpm_http_direct_conns *conns)
{
	return conns->limits.max_connections > 0 || conns->limits.max_per_client > 0;
}

static void fpm_direct_conn_unlink(struct fpm_direct_conn *c)
{
	struct fpm_http_direct_conns *conns = c->conns;

	if (c->prev) {
		c->prev->next = c->next;
	} else {
		conns->list = c->next;
	}
	if (c->next) {
		c->next->prev = c->prev;
	} else {
		conns->tail = c->prev;
	}
	c->next = NULL;
	c->prev = NULL;
}

static void fpm_direct_conn_link_front(struct fpm_http_direct_conns *conns, struct fpm_direct_conn *c)
{
	c->prev = NULL;
	c->next = conns->list;
	if (conns->list) {
		conns->list->prev = c;
	} else {
		conns->tail = c;
	}
	conns->list = c;
}

/* Precondition: the node is linked. fpm_direct_conn_unlink() below trusts
 * prev == NULL to mean "this is the head" and writes conns->list, so calling
 * this on a node that is not in the list would orphan every node after the
 * real head and leave live counting them forever -- with http.max_connections
 * that wedges the accept gate shut for the life of the child. The failure
 * paths in fpm_http_direct_conns_accepted() free their node directly for this
 * reason: they run before it is linked. */
static void fpm_direct_conn_forget(struct fpm_direct_conn *c)
{
	fpm_direct_conn_unlink(c);
	c->conns->live--;
	if (c->deadline) {
		event_free(c->deadline);
	}
	if (c->watch) {
		event_free(c->watch);
	}
	/* Last, and after both events are gone: when evhttp has already let go of
	 * this connection this is the call that frees the bufferevent and closes
	 * its fd (BEV_OPT_CLOSE_ON_FREE), and the EOF watcher must not still be
	 * registered on that fd when it goes. */
	bufferevent_decref(c->bev);
	free(c);
}

/* Ends a connection we want gone. Telling libevent its timeouts expired a
 * microsecond ago is the only way to do that from outside evhttp: the
 * connection object belongs to evhttp, which frees it on the timeout and
 * cleans up its own state on the way. Taken from the gateway's deadline
 * (fpm_http.c), where it has been the mechanism since issue #90. */
static void fpm_direct_conn_drop(struct fpm_direct_conn *c)
{
	static const struct timeval now = {0, 1};

	bufferevent_set_timeouts(c->bev, &now, &now);
}

/* Defined below, called from the per-client check and from may_accept(): both
 * ask for a count, and a count is only true once the dead nodes are gone. */
static void fpm_direct_conn_sweep_all(struct fpm_http_direct_conns *conns);

static void fpm_direct_conn_deadline_fire(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_direct_conn *c = arg;

	(void) fd;
	(void) what;
	c->conns->timed_out++;
	/* Once per child, not once per connection: the point is that an operator
	 * who did not expect this limit to bite finds out that it did, and a line
	 * per dropped client would be a log amplifier for the very flood the
	 * deadline exists to survive. How many is the status page's answer, since
	 * issue #64. */
	if (!c->conns->said_timed_out) {
		c->conns->said_timed_out = 1;
		zlog(ZLOG_NOTICE, "[pool %s] dropping connections whose first request does not arrive "
			"within http.read_timeout (%d ms); this is logged once per child",
			c->conns->limits.pool, c->conns->limits.read_timeout_ms);
	}
	fpm_direct_conn_drop(c);
	fpm_direct_conn_forget(c);
}

/* True once evhttp is done with this connection. bufferevent_free() clears
 * every callback (libevent-2.1.12 bufferevent.c:809 -- setcb(NULL, NULL, NULL,
 * NULL)) and our reference keeps the object alive past that point, so "no
 * callbacks at all" is exactly "evhttp has let go".
 *
 * All three, not just the read callback: while it writes a response evhttp
 * deliberately sets the read callback to NULL and keeps the other two
 * (http.c:382, evhttp_write_buffer, "Disable the read callback: we don't
 * actually care about data"). Asking only about readcb therefore called every
 * connection abandoned for as long as its response was being written, and the
 * sweep released the node underneath it: measured on the poligon 2026-09-12 as
 * a status page reporting `live connections: 0` on a child that had just
 * answered 730 requests on a keep-alive connection it was still holding, with
 * both of the test's connections landing on that one child because
 * may_accept() believed the same zero. */
static int fpm_direct_conn_abandoned(const struct fpm_direct_conn *c)
{
	bufferevent_data_cb readcb = NULL, writecb = NULL;
	bufferevent_event_cb eventcb = NULL;

	bufferevent_getcb(c->bev, &readcb, &writecb, &eventcb, NULL);
	return readcb == NULL && writecb == NULL && eventcb == NULL;
}

/* EV_READ on a connection whose first request has not arrived. Only a real EOF
 * releases the node: the watcher also fires for every trickle byte, and those
 * are the client's business until the deadline says otherwise. */
static void fpm_direct_conn_eof(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_direct_conn *c = arg;
	char byte;
	ssize_t n;

	(void) fd;
	if (what & EV_READ) {
		/* Checked before the peek, as in fpm_http.c: bytes nobody will ever
		 * read look exactly like a live peer, and on a level-triggered
		 * EV_READ that would spin the loop for the rest of the deadline. */
		if (!fpm_direct_conn_abandoned(c)) {
			n = recv(c->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
			if (n > 0 || (n < 0 && fpm_direct_conn_would_block(errno))) {
				return;
			}
		}
	}
	fpm_direct_conn_forget(c);
}

/* Fills in the fd and the peer, once. Both are unknowable in the bevcb --
 * libevent calls bufferevent_setfd() after it returns -- so every caller that
 * needs them has to be prepared to be the first one to ask. Returns 0 when the
 * connection has an fd. */
static int fpm_direct_conn_resolve(struct fpm_direct_conn *c)
{
	if (c->fd >= 0) {
		return 0;
	}
	c->fd = bufferevent_getfd(c->bev);
	if (c->fd < 0) {
		return -1;
	}
	c->peer_len = sizeof(c->peer);
	if (getpeername(c->fd, (struct sockaddr *) &c->peer, &c->peer_len) != 0) {
		c->peer_len = 0;
	}
	return 0;
}

static unsigned fpm_direct_conn_peer_count(struct fpm_http_direct_conns *conns,
	const struct fpm_direct_conn *self)
{
	struct fpm_direct_conn *c;
	unsigned n = 0;

	for (c = conns->list; c; c = c->next) {
		/* Resolved here and not only in the pickup pass: a connection that has
		 * not had its own pickup yet still occupies the peer, and leaving it
		 * with peer_len == 0 would let a client over its cap simply by opening
		 * its connections faster than the loop runs them. */
		if (c != self) {
			fpm_direct_conn_resolve(c);
		}
		if (c == self || c->peer_len != self->peer_len) {
			continue;
		}
		/* Compared as the kernel returned them, address and port together
		 * would be wrong here -- two connections from one client differ in
		 * the port, and the point is to count them as one client. */
		if (self->peer.ss_family == AF_INET) {
			const struct sockaddr_in *a = (const struct sockaddr_in *) &c->peer;
			const struct sockaddr_in *b = (const struct sockaddr_in *) &self->peer;
			if (a->sin_family == b->sin_family && a->sin_addr.s_addr == b->sin_addr.s_addr) {
				n++;
			}
		} else if (self->peer.ss_family == AF_INET6) {
			const struct sockaddr_in6 *a = (const struct sockaddr_in6 *) &c->peer;
			const struct sockaddr_in6 *b = (const struct sockaddr_in6 *) &self->peer;
			if (a->sin6_family == b->sin6_family &&
				!memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(a->sin6_addr))) {
				n++;
			}
		}
	}
	return n;
}

/* Judges one connection against http.max_connections_per_client, once. Returns
 * -1 when the connection was over the cap, in which case the node is gone --
 * the caller must not touch it again.
 *
 * `drop` says whether this file is also the one that ends the connection. From
 * the pickup pass it is: nobody else is going to. From inside evhttp's request
 * callback it is NOT, and must not be: fpm_direct_conn_drop() sets the write
 * timeout to 1 us as well as the read one, and the 503 the caller is about to
 * send is a write. A client whose receive window is full would have the
 * response torn down by that timer mid-send, making the refusal's shape depend
 * on the peer's TCP window. evhttp_send_error() closes the connection by
 * itself (Connection: close), which is what that path wants anyway. */
static int fpm_direct_conn_check_peer(struct fpm_direct_conn *c, int drop)
{
	if (c->checked || c->conns->limits.max_per_client <= 0) {
		return 0;
	}
	c->checked = 1;
	if (!c->peer_len) {
		/* No peer, no per-client policy: getpeername() failed, which on a
		 * connection this child is still holding means it has already ended. */
		return 0;
	}
	/* Before the count, never after. A node whose request has been served is
	 * released by the sweep, and until then it still answers to this peer:
	 * counting it would charge a client for connections it has already closed.
	 * A client fast enough to open and close more than max_per_client
	 * connections inside one 10 ms tick would otherwise refuse itself. The
	 * exhaustive walk, not the bounded one, because a count that has not
	 * looked at every node is not a count -- affordable for the same reason as
	 * in may_accept(): this path only exists when http.max_connections_per_client
	 * is set, and validation requires http.max_connections alongside it, which
	 * is what bounds the walk. The count below is a second walk of the same
	 * list, so the bound was already the price of this directive. */
	fpm_direct_conn_sweep_all(c->conns);
	if (fpm_direct_conn_peer_count(c->conns, c) < (unsigned) c->conns->limits.max_per_client) {
		return 0;
	}
	c->conns->refused++;
	if (!c->conns->said_refused) {
		c->conns->said_refused = 1;
		zlog(ZLOG_NOTICE, "[pool %s] a client reached http.max_connections_per_client (%d) "
			"on this child and its further connections are closed; this is logged once per child",
			c->conns->limits.pool, c->conns->limits.max_per_client);
	}
	if (drop) {
		fpm_direct_conn_drop(c);
	}
	fpm_direct_conn_forget(c);
	return -1;
}

/* Second pass of arming: evhttp has called bufferevent_setfd() by now, so the
 * fd -- and with it the peer -- is knowable. */
static void fpm_direct_conn_pickup(evutil_socket_t fd, short what, void *arg)
{
	struct fpm_direct_conn *c = arg;

	(void) fd;
	(void) what;
	if (fpm_direct_conn_resolve(c) < 0) {
		/* No fd and no watcher: a peer close would leave this node holding a
		 * reference until the deadline, so drop the node instead. */
		fpm_direct_conn_forget(c);
		return;
	}
	if (fpm_direct_conn_check_peer(c, 1) < 0) {
		return;
	}
	event_assign(c->watch, c->conns->base, c->fd, EV_READ | EV_PERSIST, fpm_direct_conn_eof, c);
	if (event_add(c->watch, NULL) < 0) {
		/* Without the watcher nothing would release this node before its
		 * deadline -- and past the first request there is no deadline -- so it
		 * would hold its reference, its fd and its http.max_connections slot
		 * for the life of the child. Untracked and unlimited, as on OOM. */
		fpm_direct_conn_forget(c);
	}
}

struct fpm_http_direct_conns *fpm_http_direct_conns_new(struct event_base *base,
	const struct fpm_http_direct_conns_limits *limits)
{
	struct fpm_http_direct_conns *conns = calloc(1, sizeof(*conns));

	if (!conns) {
		return NULL;
	}
	conns->base = base;
	conns->limits = *limits;
	return conns;
}

void fpm_http_direct_conns_free(struct fpm_http_direct_conns *conns)
{
	if (!conns) {
		return;
	}
	/* Re-reading conns->list each turn is the loop, not a bug: forget()
	 * unlinks before it frees, so by the time the node is gone conns->list is
	 * already the next one. clang-analyzer reports this as a use-after-free
	 * (lint-c report, two sites) because it does not follow the write through
	 * c->conns inside fpm_direct_conn_unlink(). Caching the head in a local
	 * and walking c->next would be the actual use-after-free. */
	while (conns->list) {
		/* NOLINTNEXTLINE(clang-analyzer-unix.Malloc) -- false positive, see above */
		fpm_direct_conn_forget(conns->list);
	}
	free(conns);
}

void fpm_http_direct_conns_accepted(struct fpm_http_direct_conns *conns, struct bufferevent *bev)
{
	struct fpm_direct_conn *c;
	static const struct timeval zero = {0, 0};
	struct timeval deadline;

	if (!conns || !bev) {
		return;
	}
	if (conns->limits.read_timeout_ms <= 0 && !fpm_direct_conn_limited(conns)) {
		return;
	}
	/* Every accept is also a chance to let go. A node whose connection has
	 * ended keeps its descriptor open until something releases it, and the
	 * worker's tick is up to 10 ms away; a busy pool accepts far more often
	 * than it ticks, so this bounds the descriptors by the connection rate
	 * rather than by the tick.
	 *
	 * The sweep is bounded (FPM_DIRECT_SWEEP_MAX), so this costs the same
	 * whatever the child is holding -- which is what lets it run on the accept
	 * path at all. */
	fpm_http_direct_conns_sweep(conns);
	c = calloc(1, sizeof(*c));
	if (!c) {
		/* OOM: this connection goes untracked and unlimited. The alternative
		 * -- refusing it -- would turn a transient allocation failure into a
		 * pool that serves nobody. */
		return;
	}
	c->conns = conns;
	c->bev = bev;
	c->fd = -1;
	gettimeofday(&c->accepted, NULL);
	/* The reference everything else in this file rests on. See the header. */
	bufferevent_incref(bev);
	c->watch = event_new(conns->base, -1, EV_TIMEOUT, fpm_direct_conn_pickup, c);
	if (!c->watch) {
		bufferevent_decref(bev);
		free(c);
		return;
	}
	if (conns->limits.read_timeout_ms > 0) {
		c->deadline = event_new(conns->base, -1, EV_TIMEOUT, fpm_direct_conn_deadline_fire, c);
		if (!c->deadline) {
			event_free(c->watch);
			bufferevent_decref(bev);
			free(c);
			return;
		}
	}
	fpm_direct_conn_link_front(conns, c);
	conns->live++;
	/* Either timer failing to arm leaves a node nothing can ever release: the
	 * pickup is what installs the EOF watcher and the deadline is what ends a
	 * connection that never sends a request. Such a node would keep its
	 * reference, its descriptor and its http.max_connections slot until the
	 * child dies, and enough of them would close the accept gate for good.
	 * Same answer as the allocation failures above: let this one connection go
	 * untracked rather than keep a node that cannot be collected. */
	if (event_add(c->watch, &zero) < 0) {
		fpm_direct_conn_forget(c);
		return;
	}
	if (c->deadline) {
		deadline.tv_sec = conns->limits.read_timeout_ms / 1000;
		deadline.tv_usec = (conns->limits.read_timeout_ms % 1000) * 1000;
		if (event_add(c->deadline, &deadline) < 0) {
			fpm_direct_conn_forget(c);
		}
	}
}

int fpm_http_direct_conns_request(struct fpm_http_direct_conns *conns, struct bufferevent *bev,
	struct timeval *accepted_out, unsigned *requests_out)
{
	struct fpm_direct_conn *c;

	if (!conns || !bev) {
		return 0;
	}
	for (c = conns->list; c; c = c->next) {
		if (c->bev != bev) {
			continue;
		}
		c->requests++;
		if (accepted_out) {
			*accepted_out = c->accepted;
		}
		if (requests_out) {
			*requests_out = c->requests;
		}
		/* The per-client cap is settled here and not left to the pickup pass,
		 * because the pickup is a zero-delay timer and this is the request
		 * callback: which of the two runs first inside one loop iteration is
		 * libevent's business, and it demonstrably varies. A cap that answers
		 * the request it is refusing is not a cap, so the last moment before
		 * the answer has to be a moment at which it is enforced. For a
		 * connection the pickup already judged this costs one branch. */
		fpm_direct_conn_resolve(c);
		if (fpm_direct_conn_check_peer(c, 0) < 0) {
			return -1;
		}
		if (c->deadline) {
			event_free(c->deadline);
			c->deadline = NULL;
		}
		if (!conns->limits.track_live && !fpm_direct_conn_limited(conns)) {
			/* Nothing left to track: the deadline is spent, no limit counts
			 * this connection and no gauge reports it. The node holds a
			 * bufferevent reference and therefore an fd, and past this point
			 * only the sweep would give it back -- a caller with neither a
			 * limit nor track_live has no sweep, so keeping it would leak one
			 * descriptor per connection (pool.executor = worker,
			 * fpm_http_direct_worker.c has no periodic tick). */
			fpm_direct_conn_forget(c);
			return 0;
		}
		/* The EOF watcher goes with the deadline. A persistent, level-
		 * triggered EV_READ on a connection whose bytes evhttp may leave in
		 * the socket -- which is what a paused read or a streaming response
		 * does -- would spin the event loop. Past the first request the sweep
		 * is what releases this node, one tick later at worst. */
		event_del(c->watch);
		c->served = 1;
		/* To the head: the sweep starts at the tail, so a connection that is
		 * making requests drifts away from the end that gets examined, and the
		 * connections that sit still drift towards it. That is the whole
		 * ordering policy -- the sweep is looking for connections that ended,
		 * and one that just spoke is the least likely candidate. */
		fpm_direct_conn_unlink(c);
		fpm_direct_conn_link_front(conns, c);
		return 0;
	}
	return 0;
}

int fpm_http_direct_conns_may_accept(struct fpm_http_direct_conns *conns)
{
	if (!conns || conns->limits.max_connections <= 0) {
		return 1;
	}

	if (conns->live < (unsigned) conns->limits.max_connections) {
		return 1;
	}
	/* At the cap, and live counts nodes rather than connections: a node
	 * outlives its connection until something releases it, so the gate would
	 * stay shut on behalf of connections that have already ended. The bounded
	 * sweep is not enough here -- it may not have reached them -- so this one
	 * walk is exhaustive. It is affordable because it only runs when the child
	 * is at a cap the operator configured, and http.max_connections is exactly
	 * what bounds the length of the walk. */
	fpm_direct_conn_sweep_all(conns);
	return conns->live < (unsigned) conns->limits.max_connections;
}

/* Every node, however many there are. Only for a caller that needs the count
 * to be exact right now -- see fpm_http_direct_conns_may_accept(). */
static void fpm_direct_conn_sweep_all(struct fpm_http_direct_conns *conns)
{
	struct fpm_direct_conn *c, *prev;

	for (c = conns->tail; c; c = prev) {
		prev = c->prev;
		/* Only connections past their first request: the others have the EOF
		 * watcher, which notices the same thing without waiting for a tick. */
		if (c->served && fpm_direct_conn_abandoned(c)) {
			fpm_direct_conn_forget(c);
		}
	}
}

/* How many nodes one sweep examines. The walk is not free: bufferevent_getcb()
 * takes the bufferevent's lock, and measured on the poligon 2026-09-12 (one
 * child, no http.max_connections, one busy keep-alive connection against N
 * idle ones) an exhaustive sweep on the 10 ms tick cost 9460 rps at N=0 but
 * 6708 at N=2000, against 9534 / 9686 for the same binary with the sweep
 * disabled and 9534 / 9686 for origin/main, which does not track a served
 * connection at all. A bounded sweep costs the same at every N -- measured on
 * the final binary at 9320 / 9778 / 9272 / 9426 / 9369 rps for N = 0 / 5 / 32
 * / 500 / 2000, against 9267 rps for a second N=0 pass, which is the width of
 * the noise. What it buys with the bound is time, not correctness -- a descriptor whose connection
 * ended waits at most live/FPM_DIRECT_SWEEP_MAX ticks instead of one, and
 * anything that needs an exact count asks for the exhaustive walk above. */
#define FPM_DIRECT_SWEEP_MAX 32u

void fpm_http_direct_conns_sweep(struct fpm_http_direct_conns *conns)
{
	unsigned budget;

	if (!conns) {
		return;
	}
	/* Never more than one full rotation. Without this a child holding five
	 * connections would still pay 32 examinations per sweep -- and a sweep
	 * runs on every accept as well as every tick -- where the exhaustive walk
	 * it replaced paid five. The bound exists to cap the cost on a long list,
	 * not to invent work on a short one. */
	budget = conns->live < FPM_DIRECT_SWEEP_MAX ? conns->live : FPM_DIRECT_SWEEP_MAX;
	/* From the tail, because that is where connections that have stopped
	 * making requests collect: every request moves its node to the head.
	 * A node that survives the examination goes to the head too, so repeated
	 * sweeps rotate through the whole list rather than re-examining the same
	 * few nodes. */
	/* Second clang-analyzer use-after-free report, same false positive as the
	 * one in fpm_http_direct_conns_free(): forget() unlinks before it frees, so
	 * the conns->tail this re-reads after `continue` is already the next node.
	 * See the comment there for why the analyzer misses the write. */
	while (budget-- > 0 && conns->tail) {
		struct fpm_direct_conn *c = conns->tail;

		if (c->served && fpm_direct_conn_abandoned(c)) {
			/* NOLINTNEXTLINE(clang-analyzer-unix.Malloc) -- false positive, see above */
			fpm_direct_conn_forget(c);
			continue;
		}
		fpm_direct_conn_unlink(c);
		fpm_direct_conn_link_front(conns, c);
	}
}

unsigned fpm_http_direct_conns_live_exact(struct fpm_http_direct_conns *conns)
{
	if (!conns) {
		return 0;
	}
	fpm_direct_conn_sweep_all(conns);
	return conns->live;
}

unsigned fpm_http_direct_conns_live(const struct fpm_http_direct_conns *conns)
{
	return conns ? conns->live : 0;
}

unsigned long fpm_http_direct_conns_timed_out(const struct fpm_http_direct_conns *conns)
{
	return conns ? conns->timed_out : 0;
}

unsigned long fpm_http_direct_conns_refused(const struct fpm_http_direct_conns *conns)
{
	return conns ? conns->refused : 0;
}
