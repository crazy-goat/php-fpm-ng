/* fpm-ng: pool.executor = fiber — the one seam every IO interception goes
 * through (issue #531).
 *
 * Shaped after the IO Hooks RFC (https://wiki.php.net/rfc/io_hooks, Under
 * Discussion, targets PHP 8.7), so that its three concepts have one place each:
 *
 *   operation  — struct fpm_fiber_io_op_s: what is waited on, plus an optional
 *                deadline. The RFC's Poll, Timer, Any and GetAddrInfo, and one
 *                fpm-ng kind the RFC has no counterpart for (WAKE: an
 *                in-process queue such as flock() or the session lock arbiter).
 *   completion — enum fpm_fiber_io_completion: the outcome of one operation.
 *   provider   — fpm_fiber_io_run(): one entry point that runs an operation for
 *                the current request fiber. Today its backend is the libevent
 *                scheduler in fpm_pool_fiber.c; only fpm_pool_fiber_io.c talks to
 *                it. Moving to an upstream provider means replacing that file,
 *                not the interception modules.
 *
 * An interception module (fpm_pool_fiber_xport.c, _select.c, _sleep.c,
 * _flock.c, and curl when it is written) is a client of this header and of
 * nothing else in the scheduler. Each one owns a struct fpm_fiber_intercept_s
 * and passes it to every call, which is how one interception is switched off
 * on its own (fiber.disable_interceptions, fpm_pool_fiber_intercept.c): a
 * disabled one is never installed, and the seam additionally answers
 * UNSUPPORTED to it, so a code path compiled into php-src by a patch
 * (0007 TLS, 0008 stream_select) falls back to the stock blocking call too.
 *
 * Mapping to docs/fiber_async_io.md, "The IO seam".
 */

#ifndef FPM_POOL_FIBER_IO_H
#define FPM_POOL_FIBER_IO_H 1

#include <stdbool.h>
#include <sys/time.h>

struct addrinfo;

/* Readiness events. Our own bits rather than libevent's EV_READ/EV_WRITE, so a
 * client (and a php-src patch) does not include event2/ — the RFC's
 * Poll\Event::Read/Write. */
#define FPM_FIBER_IO_READ  0x1
#define FPM_FIBER_IO_WRITE 0x2

/* The RFC's CompletionStatus, restricted to what a readiness provider in one
 * process can produce. */
enum fpm_fiber_io_completion {
	/* The operation completed: the fd is ready (POLL, ANY: see revents), the
	 * deadline of a TIMER passed, a WAKE was delivered, a GETADDRINFO has its
	 * answer (which may be an error, see gai_error). RFC: Ready / Done. */
	FPM_FIBER_IO_READY,
	/* The deadline passed first. Never returned for TIMER. */
	FPM_FIBER_IO_TIMEOUT,
	/* The wait ended before its event: a TIMER woken early. RFC: Cancelled.
	 * The libevent backend never produces it today (nothing wakes a sleeping
	 * fiber early, and a dropped request or a shutdown is never resumed), but a
	 * client must handle it: an upstream provider will. */
	FPM_FIBER_IO_CANCELLED,
	/* Nothing was suspended: not in a request fiber, a nested user Fiber, a
	 * context where the engine forbids switching, the interception is disabled,
	 * or the backend cannot do this operation (no evdns resolver). The caller
	 * performs the blocking call itself, exactly as stock PHP would. This is
	 * the old -1. RFC: Unsupported. */
	FPM_FIBER_IO_UNSUPPORTED
};

enum fpm_fiber_io_op_type {
	FPM_FIBER_IO_OP_POLL,		/* u.poll: one fd, until ready or deadline */
	FPM_FIBER_IO_OP_TIMER,		/* deadline only (timeout must be set) */
	FPM_FIBER_IO_OP_WAKE,		/* until fpm_fiber_io_wake(waker) or deadline */
	FPM_FIBER_IO_OP_ANY,		/* u.any: several fds, until one is ready or deadline */
	FPM_FIBER_IO_OP_GETADDRINFO	/* u.getaddrinfo: resolve a host name, until answer or deadline */
};

struct fpm_fiber_io_poll_s {
	int fd;
	unsigned events;	/* in:  FPM_FIBER_IO_READ | FPM_FIBER_IO_WRITE */
	unsigned revents;	/* out: the subset observed ready */
};

struct fpm_fiber_io_op_s {
	enum fpm_fiber_io_op_type type;
	struct timeval *timeout;	/* NULL = no deadline */
	union {
		struct fpm_fiber_io_poll_s poll;
		struct {
			/* An ANY whose member cannot be watched (a regular file under
			 * epoll) reports that member ready at once, as select() does on
			 * Linux. */
			struct fpm_fiber_io_poll_s *members;
			int count;
		} any;
		struct {
			const char *host;		/* in */
			struct addrinfo *res;		/* out, READY with gai_error 0; free with fpm_fiber_io_freeaddrinfo */
			int gai_error;			/* out, READY: 0 or an EVUTIL_EAI_* code for fpm_fiber_io_gai_strerror */
		} getaddrinfo;
	} u;
};

/* One interception, as the registry (fpm_pool_fiber_intercept.c) and the seam
 * see it. Owned by its module; the registry only sets `disabled`. A client
 * that is not in the registry (the session lock arbiter in
 * fpm_pool_coop_session_patch.c) is never disabled: zero-initialised means on. */
struct fpm_fiber_intercept_s {
	const char *name;		/* the name fiber.disable_interceptions takes */
	/* Called once per child, after every extension's MINIT, in table order.
	 * NULL when there is nothing to install (the patch 0008 call site is
	 * compiled in; only the switch applies). */
	void (*install)(void);
	/* Called when a request's fiber is gone, however it ended, with the waker
	 * that request had (fpm_fiber_io_waker()). NULL = no per-request state. */
	void (*request_end)(void *waker);
	/* NULL, or what disabling this entry costs beyond "blocks the whole
	 * process", logged as a WARNING next to the disabled NOTICE. flock sets it:
	 * its stock call, against a lock another request of the same process
	 * holds, never returns. */
	const char *disabled_hazard;
	bool disabled;
};

/* Could `who` suspend the current request right now? A cheap pre-check for
 * callers that must choose a code path before building an operation (the
 * stream layer, which may then skip a poll() entirely). fpm_fiber_io_run()
 * checks the same thing, so the pre-check is never required for safety. */
bool fpm_fiber_io_can_suspend(const struct fpm_fiber_intercept_s *who);

/* Run one operation for the current request fiber: suspend until it
 * completes, then return its completion. See enum fpm_fiber_io_completion for
 * what each type can return; UNSUPPORTED means nothing was suspended and the
 * caller must perform the blocking call itself. */
enum fpm_fiber_io_completion fpm_fiber_io_run(const struct fpm_fiber_intercept_s *who, struct fpm_fiber_io_op_s *op);

/* A handle naming the current request, for in-process wait queues (WAKE).
 * Obtain it BEFORE starting whatever will wake you: the waking side may run
 * synchronously, before the WAKE operation starts, and a wake with nobody
 * waiting is a no-op. NULL outside a request fiber. The same handle is passed
 * to request_end, so a module may also use it as an owner identity. */
void *fpm_fiber_io_waker(void);
void fpm_fiber_io_wake(void *waker);

/* GETADDRINFO results. */
void fpm_fiber_io_freeaddrinfo(struct addrinfo *res);
const char *fpm_fiber_io_gai_strerror(int gai_error);

#endif
