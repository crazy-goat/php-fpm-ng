/* fpm-ng: pool.executor = fiber — the IO seam and its libevent backend. See
 * fpm_pool_fiber_io.h for the contract and docs/fiber_async_io.md, "The IO
 * seam", for how it maps onto the IO Hooks RFC.
 *
 * This is the only file that turns an operation into scheduler calls
 * (fpm_pool_fiber_wait_fd/wait_wake/waiter/wake/event_base in
 * fpm_pool_fiber.c) and the only one that puts events of its own on the
 * scheduler's event_base (ANY members, evdns). Replacing the backend with an
 * upstream provider means rewriting this file; the interception modules call
 * fpm_fiber_io_run() and do not change. build/test-fiber-io-seam.sh keeps it
 * that way.
 *
 * The ANY and GETADDRINFO bodies below are the code that used to live in
 * fpm_pool_fiber_select.c and fpm_pool_fiber_xport.c, moved, not rewritten:
 * issue #531 is "no behaviour change".
 */

#include "fpm_config.h"

#include <string.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <event2/event.h>
#include <event2/dns.h>
#include <event2/util.h>

#include "php.h"

#include "fpm_pool_coop.h"
#include "fpm_pool_fiber.h"
#include "fpm_pool_fiber_io.h"
#include "zlog.h"

/* struct fpm_fiber_io_op_s hands evdns's result to the caller as a struct
 * addrinfo so that clients do not include event2/. That is only the same
 * struct when libevent was built with the system one (every Linux build). */
#ifndef EVENT__HAVE_STRUCT_ADDRINFO
# error "fpm_pool_fiber_io.c assumes libevent's evutil_addrinfo is the system struct addrinfo"
#endif

static short fpm_fiber_io_to_ev(unsigned events) /* {{{ */
{
	return (short) (((events & FPM_FIBER_IO_READ) ? EV_READ : 0) | ((events & FPM_FIBER_IO_WRITE) ? EV_WRITE : 0));
}
/* }}} */

static unsigned fpm_fiber_io_from_ev(short what) /* {{{ */
{
	return ((what & EV_READ) ? FPM_FIBER_IO_READ : 0) | ((what & EV_WRITE) ? FPM_FIBER_IO_WRITE : 0);
}
/* }}} */

bool fpm_fiber_io_can_suspend(const struct fpm_fiber_intercept_s *who) /* {{{ */
{
	if (who && who->disabled) {
		return false;
	}
	return fpm_pool_fiber_can_wait() != 0;
}
/* }}} */

void *fpm_fiber_io_waker(void) /* {{{ */
{
	return fpm_pool_fiber_waiter();
}
/* }}} */

void fpm_fiber_io_wake(void *waker) /* {{{ */
{
	fpm_pool_fiber_wake(waker);
}
/* }}} */

/* --- POLL / TIMER / WAKE: the scheduler's own primitives ------------------ */

static enum fpm_fiber_io_completion fpm_fiber_io_poll(struct fpm_fiber_io_op_s *op) /* {{{ */
{
	short what = 0;
	int w = fpm_pool_fiber_wait_fd(op->u.poll.fd, fpm_fiber_io_to_ev(op->u.poll.events), op->timeout, &what);

	op->u.poll.revents = 0;
	if (w < 0) {
		return FPM_FIBER_IO_UNSUPPORTED;
	}
	if (w == 0) {
		return FPM_FIBER_IO_TIMEOUT;
	}
	op->u.poll.revents = fpm_fiber_io_from_ev(what);
	return FPM_FIBER_IO_READY;
}
/* }}} */

/* wait_wake() with a deadline: 0 = the deadline passed, which for a TIMER is
 * its normal completion; 1 = woken before it. */
static enum fpm_fiber_io_completion fpm_fiber_io_timer(struct fpm_fiber_io_op_s *op) /* {{{ */
{
	int w = fpm_pool_fiber_wait_wake(op->timeout);

	if (w < 0) {
		return FPM_FIBER_IO_UNSUPPORTED;
	}
	return w == 0 ? FPM_FIBER_IO_READY : FPM_FIBER_IO_CANCELLED;
}
/* }}} */

static enum fpm_fiber_io_completion fpm_fiber_io_wait_wake(struct fpm_fiber_io_op_s *op) /* {{{ */
{
	int w = fpm_pool_fiber_wait_wake(op->timeout);

	if (w < 0) {
		return FPM_FIBER_IO_UNSUPPORTED;
	}
	return w == 0 ? FPM_FIBER_IO_TIMEOUT : FPM_FIBER_IO_READY;
}
/* }}} */

/* --- ANY: stream_select() ------------------------------------------------- */

/* One libevent event per member, registered on the scheduler's event_base.
 * Waking uses the external-wake pair (waiter/wait_wake/wake) instead of a new
 * state machine: whichever event fires first calls fpm_pool_fiber_wake(), and
 * wait_wake() returns as soon as any of them (or the timeout) does. Multiple
 * events firing in the same libevent loop pass is not a race (single-threaded
 * event loop); each just accumulates its own bits before the Fiber actually
 * resumes. */
struct fpm_fiber_io_any_s {
	struct fpm_fiber_io_poll_s *member;
	void *waiter;
	struct event *ev;	/* NULL once freed, or if never created (event_add failed) */
};

static void fpm_fiber_io_any_cb(evutil_socket_t fd, short what, void *arg) /* {{{ */
{
	struct fpm_fiber_io_any_s *e = arg;

	(void) fd;
	e->member->revents |= fpm_fiber_io_from_ev(what);
	fpm_pool_fiber_wake(e->waiter);
}
/* }}} */

static void fpm_fiber_io_any_free(struct fpm_fiber_io_any_s *entries, int n) /* {{{ */
{
	int i;

	for (i = 0; i < n; i++) {
		if (entries[i].ev) {
			event_del(entries[i].ev);
			event_free(entries[i].ev);
		}
	}
	efree(entries);
}
/* }}} */

static enum fpm_fiber_io_completion fpm_fiber_io_any(struct fpm_fiber_io_op_s *op) /* {{{ */
{
	struct event_base *base = fpm_pool_fiber_event_base();
	struct fpm_fiber_io_any_s *entries;
	void *waiter;
	bool any_ready = false;
	int i, n = op->u.any.count;

	if (!base || n <= 0) {
		return FPM_FIBER_IO_UNSUPPORTED;
	}

	entries = ecalloc(n, sizeof(*entries));
	waiter = fpm_pool_fiber_waiter();

	for (i = 0; i < n; i++) {
		struct fpm_fiber_io_any_s *e = &entries[i];
		struct fpm_fiber_io_poll_s *m = &op->u.any.members[i];

		m->revents = 0;
		e->member = m;
		e->waiter = waiter;
		e->ev = event_new(base, m->fd, fpm_fiber_io_to_ev(m->events), fpm_fiber_io_any_cb, e);
		if (!e->ev || event_add(e->ev, NULL) < 0) {
			/* Not watchable through epoll (an ordinary file, most likely) or
			 * out of memory: select() reports a regular file as always ready
			 * on Linux, so do the same rather than waiting on it forever. */
			if (e->ev) {
				event_free(e->ev);
				e->ev = NULL;
			}
			m->revents = m->events;
		}
		if (m->revents) {
			any_ready = true;
		}
	}

	/* A member already resolved (the not-watchable case above) before we ever
	 * call wait_wake(): a wake arriving before the wait starts is a no-op
	 * (fpm_pool_fiber.h), so waiting here would lose it and block for the full
	 * timeout instead of returning promptly, as select() would for a
	 * descriptor that is already ready. */
	if (!any_ready && fpm_pool_fiber_wait_wake(op->timeout) < 0) {
		/* can_wait() changed its mind between the check in run() and here
		 * (should not happen in single-threaded code): report that nothing
		 * was waited for, so the caller does the real, blocking select(). */
		fpm_fiber_io_any_free(entries, n);
		for (i = 0; i < n; i++) {
			op->u.any.members[i].revents = 0;
		}
		return FPM_FIBER_IO_UNSUPPORTED;
	}

	fpm_fiber_io_any_free(entries, n);
	for (i = 0; i < n; i++) {
		if (op->u.any.members[i].revents) {
			return FPM_FIBER_IO_READY;
		}
	}
	return FPM_FIBER_IO_TIMEOUT;
}
/* }}} */

/* --- GETADDRINFO: evdns on the scheduler's event_base --------------------- */

static struct evdns_base *fpm_fiber_dns;
static bool fpm_fiber_dns_tried;

/* evdns logs every query ("Resolve requested for", "Sending request for ...
 * on ipv4/ipv6") and nameserver failures; with log_level = debug it is
 * therefore visible which connects used DNS and which did not. */
static void fpm_fiber_dns_log(int is_warning, const char *msg) /* {{{ */
{
	zlog(is_warning ? ZLOG_WARNING : ZLOG_DEBUG, "[pool %s] fiber: evdns: %s", fpm_coop_pool_name(), msg);
}
/* }}} */

/* evdns base, once per process, initialized lazily on the first lookup.
 * NULL = no resolver; GETADDRINFO is then UNSUPPORTED and the caller uses
 * blocking getaddrinfo. */
static struct evdns_base *fpm_fiber_dns_base(void) /* {{{ */
{
	struct event_base *base;

	if (fpm_fiber_dns_tried) {
		return fpm_fiber_dns;
	}
	fpm_fiber_dns_tried = true;

	base = fpm_pool_fiber_event_base();
	if (!base) {
		return NULL;
	}
	evdns_set_log_fn(fpm_fiber_dns_log);
	/* INITIALIZE_NAMESERVERS = DNS_OPTIONS_ALL: nameserver/search/ndots from
	 * /etc/resolv.conf AND /etc/hosts. Without resolv.conf or without any
	 * nameserver, evdns_base_new returns NULL — the blocking path remains, which
	 * also has nothing to query in that situation. */
	fpm_fiber_dns = evdns_base_new(base, EVDNS_BASE_INITIALIZE_NAMESERVERS | EVDNS_BASE_DISABLE_WHEN_INACTIVE);
	if (!fpm_fiber_dns) {
		zlog(ZLOG_WARNING, "[pool %s] fiber: evdns_base_new() failed (no /etc/resolv.conf or no nameservers?); DNS stays blocking",
			fpm_coop_pool_name());
		return NULL;
	}
	/* getaddrinfo does not use 0x20 randomization; resolvers that do not preserve
	 * letter case in the response would have their response rejected by evdns. */
	evdns_base_set_option(fpm_fiber_dns, "randomize-case", "0");

	zlog(ZLOG_DEBUG, "[pool %s] fiber: async DNS via evdns, %d nameserver(s)",
		fpm_coop_pool_name(), evdns_base_count_nameservers(fpm_fiber_dns));
	return fpm_fiber_dns;
}
/* }}} */

struct fpm_fiber_dns_req_s {
	void *waiter;
	struct evutil_addrinfo *res;
	int result;			/* EVUTIL_EAI_* code (0 = ok) */
	bool done;
};

static void fpm_fiber_dns_cb(int result, struct evutil_addrinfo *res, void *arg) /* {{{ */
{
	struct fpm_fiber_dns_req_s *r = arg;

	r->result = result;
	r->res = res;
	r->done = true;
	fpm_pool_fiber_wake(r->waiter);
}
/* }}} */

/* Stream (TCP) addresses of any family, the hints the "tcp" transport's
 * getaddrinfo() uses (php_network_getaddresses). */
static enum fpm_fiber_io_completion fpm_fiber_io_getaddrinfo(struct fpm_fiber_io_op_s *op) /* {{{ */
{
	struct evdns_base *dns = fpm_fiber_dns_base();
	struct fpm_fiber_dns_req_s r = { NULL, NULL, 0, false };
	struct evutil_addrinfo hints;
	struct evdns_getaddrinfo_request *req;
	int w = 1;

	op->u.getaddrinfo.res = NULL;
	op->u.getaddrinfo.gai_error = 0;
	if (!dns) {
		return FPM_FIBER_IO_UNSUPPORTED;
	}
	r.waiter = fpm_pool_fiber_waiter();

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	/* A hit in /etc/hosts or an immediate error: the callback arrives
	 * synchronously, req == NULL, and r.done is already set. */
	req = evdns_getaddrinfo(dns, op->u.getaddrinfo.host, NULL, &hints, fpm_fiber_dns_cb, &r);
	if (!r.done) {
		w = fpm_pool_fiber_wait_wake(op->timeout);
		if (!r.done) {
			/* Timeout or inability to wait: cancel calls the callback synchronously
			 * with EVUTIL_EAI_CANCEL, so r is no longer in use. */
			evdns_getaddrinfo_cancel(req);
			return w == 0 ? FPM_FIBER_IO_TIMEOUT : FPM_FIBER_IO_UNSUPPORTED;
		}
	}
	op->u.getaddrinfo.gai_error = r.result;
	if (r.result == 0) {
		op->u.getaddrinfo.res = r.res;
	}
	return FPM_FIBER_IO_READY;
}
/* }}} */

void fpm_fiber_io_freeaddrinfo(struct addrinfo *res) /* {{{ */
{
	if (res) {
		evutil_freeaddrinfo(res);
	}
}
/* }}} */

const char *fpm_fiber_io_gai_strerror(int gai_error) /* {{{ */
{
	return evutil_gai_strerror(gai_error);
}
/* }}} */

/* --- the entry point ------------------------------------------------------- */

enum fpm_fiber_io_completion fpm_fiber_io_run(const struct fpm_fiber_intercept_s *who, struct fpm_fiber_io_op_s *op) /* {{{ */
{
	if (!fpm_fiber_io_can_suspend(who)) {
		return FPM_FIBER_IO_UNSUPPORTED;
	}
	switch (op->type) {
		case FPM_FIBER_IO_OP_POLL:
			return fpm_fiber_io_poll(op);
		case FPM_FIBER_IO_OP_TIMER:
			return fpm_fiber_io_timer(op);
		case FPM_FIBER_IO_OP_WAKE:
			return fpm_fiber_io_wait_wake(op);
		case FPM_FIBER_IO_OP_ANY:
			return fpm_fiber_io_any(op);
		case FPM_FIBER_IO_OP_GETADDRINFO:
			return fpm_fiber_io_getaddrinfo(op);
	}
	return FPM_FIBER_IO_UNSUPPORTED;
}
/* }}} */
