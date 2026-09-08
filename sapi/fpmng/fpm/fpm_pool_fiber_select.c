/* fpm-ng: pool.executor = fiber — make stream_select() suspend the request
 * Fiber instead of blocking the process. See fpm_pool_fiber_select.h and
 * task 006 (done; see docs/task-archive.md).
 *
 * stream_select() takes SETS of descriptors from userland
 * (ext/standard/streamsfuncs.c: PHP_FUNCTION(stream_select)), unlike the
 * per-stream interception in fpm_pool_fiber_xport.c. The patch
 * (patches/0008-fiber-stream-select.patch, HAVE_FPMNG_FIBER) replaces exactly one
 * call, php_select(max_fd+1, &rfds, &wfds, &efds, tv_p), with
 * fpm_fiber_select() below — same signature, same postconditions. Everything
 * else in that function (argument parsing, building rfds/wfds/efds from the
 * stream arrays, narrowing them back afterwards) is untouched upstream code.
 *
 * Mechanism: one libevent event per candidate fd, registered on the
 * scheduler's event_base (fpm_pool_fiber_event_base()) exactly like the evdns
 * requests in fpm_pool_fiber_xport.c. Waking uses the existing external-wake
 * pair (fpm_pool_fiber_waiter/wait_wake/wake) instead of a new state machine:
 * whichever event fires first calls fpm_pool_fiber_wake(), and wait_wake()
 * returns as soon as any of them (or the timeout) does. Multiple events firing
 * in the same libevent loop pass is not a race (single-threaded event loop);
 * each just accumulates its own bits before the Fiber actually resumes.
 *
 * Not handled here, on purpose:
 * - efds (the "exceptional conditions" set): select()'s meaning for it is
 *   mostly OOB TCP data, which libevent has no portable equivalent for.
 *   Guessing at it would violate the "never change what the function reports"
 *   acceptance criterion, so a non-empty efds falls back to the real,
 *   process-blocking select() — same as calling stream_select() would have
 *   done before this file existed. Predis and php-amqplib do not use it.
 * - fd_select on the async executor: tracked separately (see task 006).
 */

#include "fpm_config.h"

#include <errno.h>
#include <string.h>
#include <sys/select.h>

#include <event2/event.h>

#include "php.h"

#include "fpm_pool_coop.h"
#include "fpm_pool_fiber.h"
#include "fpm_pool_fiber_select.h"
#include "zlog.h"

struct fpm_fiber_select_entry {
	int fd;
	short want;		/* EV_READ and/or EV_WRITE requested for this fd */
	short got;		/* bits actually observed ready */
	void *waiter;
	struct event *ev;	/* NULL once freed, or if never created (event_add failed) */
};

static void fpm_fiber_select_cb(evutil_socket_t fd, short what, void *arg) /* {{{ */
{
	struct fpm_fiber_select_entry *e = arg;

	(void) fd;
	e->got |= what;
	fpm_pool_fiber_wake(e->waiter);
}
/* }}} */

/* Once per process: efds is rare enough (neither Predis nor php-amqplib use
 * it) that falling back silently every time would be surprising in the log,
 * but logging every call would spam it under a library that does pass it. */
static bool fpm_fiber_select_efds_warned = false;

int fpm_fiber_select(int max_fd, fd_set *rfds, fd_set *wfds, fd_set *efds, struct timeval *timeout) /* {{{ */
{
	struct event_base *base;
	struct fpm_fiber_select_entry *entries = NULL;
	void *waiter;
	int fd, n = 0, ready;
	bool poll_only = timeout && timeout->tv_sec == 0 && timeout->tv_usec == 0;
	bool have_efds = false;

	if (efds) {
		for (fd = 0; fd < max_fd; fd++) {
			if (FD_ISSET(fd, efds)) {
				have_efds = true;
				break;
			}
		}
	}

	if (have_efds || poll_only || !fpm_pool_fiber_can_wait()) {
		if (have_efds && !fpm_fiber_select_efds_warned) {
			fpm_fiber_select_efds_warned = true;
			zlog(ZLOG_DEBUG, "[pool %s] fiber: stream_select() with a non-empty "
				"exceptfds set falls back to blocking select() (once per process)",
				fpm_coop_pool_name());
		}
		return select(max_fd, rfds, wfds, efds, timeout);
	}

	base = fpm_pool_fiber_event_base();
	if (!base) {
		return select(max_fd, rfds, wfds, efds, timeout);
	}

	entries = ecalloc(max_fd, sizeof(*entries));
	waiter = fpm_pool_fiber_waiter();

	for (fd = 0; fd < max_fd; fd++) {
		short want = 0;
		struct fpm_fiber_select_entry *e;

		if (rfds && FD_ISSET(fd, rfds)) {
			want |= EV_READ;
		}
		if (wfds && FD_ISSET(fd, wfds)) {
			want |= EV_WRITE;
		}
		if (!want) {
			continue;
		}

		e = &entries[n++];
		e->fd = fd;
		e->want = want;
		e->waiter = waiter;
		e->ev = event_new(base, fd, want, fpm_fiber_select_cb, e);
		if (!e->ev || event_add(e->ev, NULL) < 0) {
			/* Not watchable through epoll (an ordinary file, most likely) or
			 * out of memory: select() reports a regular file as always ready
			 * on Linux, so do the same rather than waiting on it forever. */
			if (e->ev) {
				event_free(e->ev);
				e->ev = NULL;
			}
			e->got = want;
		}
	}

	if (n == 0) {
		/* stream_array_to_fd_set already refused an empty set of arrays
		 * before this is reached; this is defensive, not a real path. */
		efree(entries);
		return select(max_fd, rfds, wfds, efds, timeout);
	}

	{
		bool any_ready = false;
		int i;

		for (i = 0; i < n; i++) {
			if (entries[i].got) {
				any_ready = true;
				break;
			}
		}

		/* An entry already resolved (the not-watchable-through-epoll case
		 * above) before we ever call wait_wake(): a wake arriving before the
		 * wait starts is a documented no-op (fpm_pool_fiber.h), so waiting
		 * here would lose it and block for the full timeout instead of
		 * returning promptly, as select() would for a descriptor that is
		 * already ready. */
		if (!any_ready) {
			int w = fpm_pool_fiber_wait_wake(timeout);

			if (w < 0) {
				/* can_wait() changed its mind between the check above and
				 * here (should not happen in single-threaded code) — do not
				 * lose the wait, fall back to the real, blocking select(). */
				for (i = 0; i < n; i++) {
					if (entries[i].ev) {
						event_del(entries[i].ev);
						event_free(entries[i].ev);
					}
				}
				efree(entries);
				return select(max_fd, rfds, wfds, efds, timeout);
			}
		}
	}

	if (rfds) {
		FD_ZERO(rfds);
	}
	if (wfds) {
		FD_ZERO(wfds);
	}
	if (efds) {
		FD_ZERO(efds);
	}

	ready = 0;
	{
		int i;

		for (i = 0; i < n; i++) {
			struct fpm_fiber_select_entry *e = &entries[i];

			if (e->ev) {
				event_del(e->ev);
				event_free(e->ev);
			}
			if ((e->got & EV_READ) && rfds) {
				FD_SET(e->fd, rfds);
				ready++;
			}
			if ((e->got & EV_WRITE) && wfds) {
				FD_SET(e->fd, wfds);
				ready++;
			}
		}
	}

	efree(entries);
	return ready;
}
/* }}} */
