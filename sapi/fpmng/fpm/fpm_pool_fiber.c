/* fpm-ng: pool.executor = fiber — libevent scheduler + engine Fibers.
 * See fpm_pool_fiber.h and docs/NOTES.md 3u.
 *
 * Who switches: ONLY this file, from the main context (the libevent loop).
 * Therefore we do not need the fork's switch handlers — request state enters
 * the globals immediately before zend_fiber_start/resume and leaves immediately
 * after they return (fpm_coop_req_enter/leave). The request Fiber suspends only
 * through fpm_pool_fiber_wait_fd()/wait_wake() (from fpm_pool_fiber_xport.c) and
 * always returns here.
 */

#include "fpm_config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <event2/event.h>

#include "php.h"
#include "zend_fibers.h"
#include "zend_exceptions.h"

#include "fpm.h"
#include "fpm_conf.h"
#include "fpm_worker_pool.h"
#include "fpm_pool_coop.h"
#include "fpm_pool_coop_reval.h"
#include "fpm_pool_fiber.h"
#include "fpm_pool_fiber_flock.h"
#include "fpm_pool_fiber_sleep.h"
#include "fpm_stdio.h"
#include "zlog.h"

int fpm_pool_fiber_validate(struct fpm_worker_pool_s *wp) /* {{{ */
{
	return fpm_coop_validate(wp, "fiber");
}
/* }}} */

#ifdef ZTS

void fpm_pool_fiber_child_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	zlog(ZLOG_ALERT, "[pool %s] pool.executor = fiber: ZTS build, this should have been rejected by validate()",
		wp->config->name);
	exit(FPM_EXIT_SOFTWARE);
}
/* }}} */

int fpm_pool_fiber_can_wait(void) { return 0; }
int fpm_pool_fiber_wait_fd(int fd, short events, struct timeval *timeout) { (void) fd; (void) events; (void) timeout; return -1; }
void *fpm_pool_fiber_waiter(void) { return NULL; }
int fpm_pool_fiber_wait_wake(struct timeval *timeout) { (void) timeout; return -1; }
void fpm_pool_fiber_wake(void *waiter) { (void) waiter; }
struct event_base *fpm_pool_fiber_event_base(void) { return NULL; }

#else /* !ZTS */

/* In-flight request from the scheduler's point of view. */
struct fpm_fiber_req_s {
	struct fpm_coop_req_s *ctx;
	zend_fiber *fiber;
	struct event *ev;			/* one I/O event per request, attached for each wait */
	short wait_result;			/* what woke it: EV_READ/EV_WRITE/EV_TIMEOUT */
	bool waiting;
};

/* Keep-alive connection between requests: wait for the next request. A list
 * lets drain (see below) close them all at once instead of waiting up to 30 s
 * for the idle timeout. */
struct fpm_fiber_kept_s {
	fcgi_request *req;
	int fd;
	struct event *ev;
	struct fpm_fiber_kept_s *prev, *next;
};

static struct event_base *fpm_fiber_base;
static struct event *fpm_fiber_ev_accept;
static struct event *fpm_fiber_ev_tick;
static struct event *fpm_fiber_ev_reval;
static int fpm_fiber_listen_fd = -1;
static struct fpm_fiber_kept_s *fpm_fiber_kept_head;

/* Drain: fiber.revalidate_freq detected a change in an included file. Do not
 * accept new connections; in-flight requests finish normally, and when none
 * remain the process exits with FPM_EXIT_OK and the master replaces it
 * (fpm_children_bury: restart_child = 1 for every exit other than idle_kill,
 * the same path that replaces a classic worker after pm.max_requests).
 * Unlike fcgi_in_shutdown() (SIGQUIT), NOTHING is abandoned. */
static bool fpm_fiber_draining = false;

/* Request whose Fiber is currently on the processor (NULL in the event loop). */
static struct fpm_fiber_req_s *fpm_fiber_current;
/* Request whose Fiber is starting (passing ctx to the entry function). */
static struct fpm_fiber_req_s *fpm_fiber_starting;

/* Internal function serving as the Fiber's "callable". A Fiber is a PHP object
 * and needs fci/fci_cache; with fci_cache.function_handler set to a ready
 * zend_function, zend_call_function does not look anything up by name. Zero
 * arguments, zero arg_info, no registration in the function table — invisible
 * to PHP. */
static void fpm_fiber_entry_handler(INTERNAL_FUNCTION_PARAMETERS);
static zend_internal_function fpm_fiber_entry_fn;

/* Keep-alive: idle deadline on a connection without a request. */
static const struct timeval fpm_fiber_keep_idle = { 30, 0 };

static void fpm_fiber_start_request(fcgi_request *req, int fd);
static void fpm_fiber_kept_cb(evutil_socket_t fd, short what, void *arg);

/* --- fiber requestu ---------------------------------------------------------- */

static void fpm_fiber_entry_handler(INTERNAL_FUNCTION_PARAMETERS) /* {{{ */
{
	struct fpm_fiber_req_s *fr = fpm_fiber_starting;

	(void) execute_data;
	fpm_fiber_starting = NULL;
	fpm_coop_req_run(fr->ctx);
	RETURN_NULL();
}
/* }}} */

static zend_fiber *fpm_fiber_create(void) /* {{{ */
{
	zend_object *obj = zend_ce_fiber->create_object(zend_ce_fiber);
	zend_fiber *fiber = (zend_fiber *) obj;

	fiber->fci.size = sizeof(fiber->fci);
	ZVAL_UNDEF(&fiber->fci.function_name);
	fiber->fci.retval = NULL;
	fiber->fci.params = NULL;
	fiber->fci.object = NULL;
	fiber->fci.param_count = 0;
	fiber->fci.named_params = NULL;
	memset(&fiber->fci_cache, 0, sizeof(fiber->fci_cache));
	fiber->fci_cache.function_handler = (zend_function *) &fpm_fiber_entry_fn;
	return fiber;
}
/* }}} */

/* After every return from a Fiber: did it finish? Clean up and handle the connection. */
static void fpm_fiber_after_switch(struct fpm_fiber_req_s *fr) /* {{{ */
{
	fcgi_request *req;
	int fd;

	if (fr->fiber->context.status != ZEND_FIBER_STATUS_DEAD) {
		if (!fr->waiting) {
			/* The Fiber suspended NOT through our wait_fd (for example,
			 * Fiber::suspend() from user code in the request's main Fiber). Nobody
			 * will wake it — treat this as a request ending with an error. */
			zlog(ZLOG_WARNING, "[pool %s] fiber: request #%u suspended outside the scheduler (Fiber::suspend() in the request's main fiber?); dropping it",
				fpm_coop_pool_name(), fr->ctx->id);
			/* SPIKE: this request will never reach release_owner() from
			 * fpm_coop_req_run() again (it will not return there). If it held an
			 * flock() from the registry, it would remain there FOREVER and block
			 * every future competitor in this process. This is EXACTLY the gap
			 * that fpm_pool_fiber_flock_release_owner() closes — see its header. */
			fpm_pool_fiber_flock_release_owner(fr);
			fcgi_finish_request(fr->ctx->req, 1);
			fr->ctx->req = NULL;
			/* Release the Fiber object when the process exits — destroying it in
			 * the SUSPENDED state resumes it with graceful exit and the WRONG global
			 * state. */
			return;
		}
		return;
	}

	fd = fr->ctx->fd;
	/* SPIKE: normal request completion (including a fatal error from inside
	 * zend_catch in fpm_coop_req_run — it still returns here in exactly the same
	 * way). Belt-and-suspenders and idempotent: no-op when fr holds nothing. */
	fpm_pool_fiber_flock_release_owner(fr);
	req = fpm_coop_req_free(fr->ctx);
	event_free(fr->ev);
	OBJ_RELEASE(&fr->fiber->std);
	efree(fr);

	if (fpm_fiber_draining) {
		/* The last in-flight request finished — we can exit. Do not keep the
		 * connection: the next request will reach the replacement. */
		if (!fcgi_is_closed(req)) {
			fcgi_finish_request(req, 1);
		}
		fcgi_destroy_request(req);
		if (fpm_coop_in_flight() == 0) {
			event_base_loopbreak(fpm_fiber_base);
		}
		return;
	}

	if (fcgi_is_closed(req)) {
		fcgi_destroy_request(req);
		return;
	}

	/* The client wants keep-alive (the HTTP gateway does this): wait for the next
	 * request on this fd in the event loop, without blocking. */
	{
		struct fpm_fiber_kept_s *kept = emalloc(sizeof(*kept));

		kept->req = req;
		kept->fd = fd;
		kept->ev = event_new(fpm_fiber_base, fd, EV_READ, fpm_fiber_kept_cb, kept);
		event_add(kept->ev, &fpm_fiber_keep_idle);
		kept->prev = NULL;
		kept->next = fpm_fiber_kept_head;
		if (fpm_fiber_kept_head) {
			fpm_fiber_kept_head->prev = kept;
		}
		fpm_fiber_kept_head = kept;
	}
}
/* }}} */

static void fpm_fiber_kept_unlink(struct fpm_fiber_kept_s *kept) /* {{{ */
{
	if (kept->prev) {
		kept->prev->next = kept->next;
	} else {
		fpm_fiber_kept_head = kept->next;
	}
	if (kept->next) {
		kept->next->prev = kept->prev;
	}
	event_free(kept->ev);
	efree(kept);
}
/* }}} */

static void fpm_fiber_kept_cb(evutil_socket_t fd, short what, void *arg) /* {{{ */
{
	struct fpm_fiber_kept_s *kept = arg;
	fcgi_request *req = kept->req;
	int new_fd = kept->fd;

	(void) fd;
	fpm_fiber_kept_unlink(kept);

	if (!(what & EV_READ)) {
		/* idle timeout */
		fcgi_finish_request(req, 1);
		fcgi_destroy_request(req);
		return;
	}

	req = fpm_coop_accept_kept(req, &new_fd);
	if (req) {
		fpm_fiber_start_request(req, new_fd);
	}
}
/* }}} */

/* Enter the processor: request state -> globals, start/resume, state <- globals. */
static void fpm_fiber_switch_in(struct fpm_fiber_req_s *fr, bool start) /* {{{ */
{
	zval rv;

	fr->waiting = false;
	fpm_fiber_current = fr;
	fpm_coop_req_enter(fr->ctx);

	ZVAL_UNDEF(&rv);
	zend_try {
		if (start) {
			fpm_fiber_starting = fr;
			if (zend_fiber_start(fr->fiber, &rv) == FAILURE) {
				zlog(ZLOG_ERROR, "[pool %s] fiber: zend_fiber_start() failed (fiber.stack_size?)", fpm_coop_pool_name());
			}
		} else {
			zend_fiber_resume(fr->fiber, NULL, &rv);
		}
	} zend_catch {
		/* A bailout escaped the Fiber (zend_fiber_switch_to passes it further).
		 * fpm_coop_req_run has its own zend_try, so this is a failure. */
		zlog(ZLOG_ERROR, "[pool %s] fiber: bailout escaped request #%u", fpm_coop_pool_name(), fr->ctx->id);
	} zend_end_try();
	zval_ptr_dtor(&rv);
	if (EG(exception)) {
		/* The Fiber threw (it should not: run() cleans up) — do not leave this
		 * for the main context, which has no frame. */
		zend_clear_exception();
	}

	fpm_coop_req_leave(fr->ctx);
	fpm_fiber_current = NULL;

	fpm_fiber_after_switch(fr);
}
/* }}} */

static void fpm_fiber_io_cb(evutil_socket_t fd, short what, void *arg) /* {{{ */
{
	struct fpm_fiber_req_s *fr = arg;

	(void) fd;
	fr->wait_result = what;
	fpm_fiber_switch_in(fr, false);
}
/* }}} */

static void fpm_fiber_start_request(fcgi_request *req, int fd) /* {{{ */
{
	struct fpm_fiber_req_s *fr = ecalloc(1, sizeof(*fr));

	fr->ctx = fpm_coop_req_new(req, fd);
	fr->ctx->type_data = fr;
	fr->fiber = fpm_fiber_create();
	fr->ev = event_new(fpm_fiber_base, -1, 0, fpm_fiber_io_cb, fr);

	fpm_fiber_switch_in(fr, true);
}
/* }}} */

/* --- transport API ---------------------------------------------------------- */

int fpm_pool_fiber_can_wait(void) /* {{{ */
{
	struct fpm_fiber_req_s *fr = fpm_fiber_current;

	if (!fr || !fpm_fiber_base) {
		return 0;
	}
	/* Nested user Fiber: zend_fiber_suspend would suspend THAT Fiber to its
	 * caller, not our request to the scheduler. Block in that case. */
	if (EG(active_fiber) != fr->fiber) {
		return 0;
	}
	/* Destructors during GC, ticks, and pcntl: the engine forbids switches. */
	if (zend_fiber_switch_blocked()) {
		return 0;
	}
	return 1;
}
/* }}} */

int fpm_pool_fiber_wait_fd(int fd, short events, struct timeval *timeout) /* {{{ */
{
	struct fpm_fiber_req_s *fr = fpm_fiber_current;
	zval rv;

	if (!fpm_pool_fiber_can_wait()) {
		return -1;
	}

	/* event_add calculates the deadline from the time cached at the start of
	 * the loop iteration. The Fiber may have blocked since then (getaddrinfo,
	 * usleep, computation) — without refreshing it, 60 ms of work consumes a
	 * 50 ms timeout and the wait ends immediately with "Operation timed out". */
	event_base_update_cache_time(fpm_fiber_base);
	if (event_assign(fr->ev, fpm_fiber_base, fd, events, fpm_fiber_io_cb, fr) < 0
		|| event_add(fr->ev, timeout) < 0) {
		return -1;
	}
	fr->wait_result = 0;
	fr->waiting = true;

	/* Back to the scheduler. We return here from fpm_fiber_io_cb ->
	 * zend_fiber_resume, with the request state already switched back
	 * (switch_in performs enter). */
	ZVAL_UNDEF(&rv);
	zend_fiber_suspend(fr->fiber, NULL, &rv);
	zval_ptr_dtor(&rv);

	event_del(fr->ev);
	if (fr->wait_result & EV_TIMEOUT) {
		return 0;
	}
	return 1;
}
/* }}} */

void *fpm_pool_fiber_waiter(void) /* {{{ */
{
	return fpm_fiber_current;
}
/* }}} */

/* Like wait_fd, but the event has no fd — only a timer. fpm_pool_fiber_wake
 * wakes it through event_active (which also works for an event without a
 * timeout, that is, one that is not pending). */
int fpm_pool_fiber_wait_wake(struct timeval *timeout) /* {{{ */
{
	struct fpm_fiber_req_s *fr = fpm_fiber_current;
	zval rv;

	if (!fpm_pool_fiber_can_wait()) {
		return -1;
	}

	event_base_update_cache_time(fpm_fiber_base);	/* jak w wait_fd */
	if (event_assign(fr->ev, fpm_fiber_base, -1, 0, fpm_fiber_io_cb, fr) < 0
		|| (timeout && event_add(fr->ev, timeout) < 0)) {
		return -1;
	}
	fr->wait_result = 0;
	fr->waiting = true;

	ZVAL_UNDEF(&rv);
	zend_fiber_suspend(fr->fiber, NULL, &rv);
	zval_ptr_dtor(&rv);

	event_del(fr->ev);
	if (fr->wait_result & EV_TIMEOUT) {
		return 0;
	}
	return 1;
}
/* }}} */

void fpm_pool_fiber_wake(void *waiter) /* {{{ */
{
	struct fpm_fiber_req_s *fr = waiter;

	/* Outside a wait (a synchronous callback, or a Fiber already woken by a
	 * timeout) there is nobody to wake. If the timer and wake arrive in the same
	 * loop iteration, event_active adds EV_READ to the already active event — one
	 * callback, with both bits in wait_result. */
	if (fr && fr->waiting) {
		event_active(fr->ev, EV_READ, 0);
	}
}
/* }}} */

struct event_base *fpm_pool_fiber_event_base(void) /* {{{ */
{
	return fpm_fiber_base;
}
/* }}} */

/* --- event loop -------------------------------------------------------------- */

static void fpm_fiber_accept_cb(evutil_socket_t fd, short what, void *arg) /* {{{ */
{
	fcgi_request *req;
	int conn_fd = -1;

	(void) fd; (void) what; (void) arg;

	if (fcgi_in_shutdown()) {
		event_base_loopbreak(fpm_fiber_base);
		return;
	}
	if (fpm_fiber_draining) {
		return;
	}
	req = fpm_coop_accept(fpm_fiber_listen_fd, &conn_fd);
	if (!req) {
		return;
	}
	fpm_fiber_start_request(req, conn_fd);
}
/* }}} */

static void fpm_fiber_tick_cb(evutil_socket_t fd, short what, void *arg) /* {{{ */
{
	(void) fd; (void) what; (void) arg;

	/* SIGQUIT: fpm_signals.c sig_soft_quit closes the listening socket and
	 * calls fcgi_terminate(); an event on the closed fd may never arrive. */
	if (fcgi_in_shutdown()) {
		event_base_loopbreak(fpm_fiber_base);
	}
	if (fpm_fiber_draining) {
		zlog(ZLOG_DEBUG, "[pool %s] fiber: draining, %u request(s) still in flight",
			fpm_coop_pool_name(), fpm_coop_in_flight());
	}
}
/* }}} */

/* Enter draining — see the comment next to fpm_fiber_draining. */
static void fpm_fiber_drain_begin(void) /* {{{ */
{
	fpm_fiber_draining = true;
	event_del(fpm_fiber_ev_accept);
	if (fpm_fiber_ev_reval) {
		event_del(fpm_fiber_ev_reval);
	}

	/* Idle keep-alive connections: close them immediately. The HTTP gateway
	 * treats EOF on an idle connection like the EOF after pm.max_requests
	 * (fpm_http_upstream_fail, clean_eof) and reconnects to the replacement. */
	while (fpm_fiber_kept_head) {
		struct fpm_fiber_kept_s *kept = fpm_fiber_kept_head;

		fcgi_finish_request(kept->req, 1);
		fcgi_destroy_request(kept->req);
		fpm_fiber_kept_unlink(kept);
	}

	if (fpm_coop_in_flight() == 0) {
		event_base_loopbreak(fpm_fiber_base);
	}
}
/* }}} */

/* Every fiber.revalidate_freq seconds, from the event loop (no request is on
 * the processor then): one stat() sweep over the tracked files. This is the
 * only place where stat() runs after file registration — the cost is a function
 * of time and the number of files, not the number of requests. */
static void fpm_fiber_reval_cb(evutil_socket_t fd, short what, void *arg) /* {{{ */
{
	const char *path = NULL;
	char why[160];
	unsigned files, sweeps, stats;

	(void) fd; (void) what; (void) arg;

	if (fpm_fiber_draining || fcgi_in_shutdown()) {
		return;
	}
	if (fpm_coop_reval_sweep(&path, why, sizeof(why))) {
		fpm_coop_reval_stats(&files, &sweeps, &stats);
		zlog(ZLOG_NOTICE, "[pool %s] fiber: %s changed on disk (%s); worker will exit after %u request(s) in flight finish, master respawns it on fresh code",
			fpm_coop_pool_name(), path, why, fpm_coop_in_flight());
		fpm_fiber_drain_begin();
		return;
	}
	fpm_coop_reval_stats(&files, &sweeps, &stats);
	zlog(ZLOG_DEBUG, "[pool %s] fiber: revalidate sweep #%u, %u file(s) unchanged, %u stat() since start",
		fpm_coop_pool_name(), sweeps, files, stats);
}
/* }}} */

void fpm_pool_fiber_child_main(struct fpm_worker_pool_s *wp) /* {{{ */
{
	struct timeval tick = { 1, 0 };

	fpm_fiber_listen_fd = fpm_globals.listening_socket;

	fpm_fiber_base = event_base_new();
	if (!fpm_fiber_base) {
		zlog(ZLOG_ERROR, "[pool %s] fiber: event_base_new() failed", wp->config->name);
		exit(FPM_EXIT_SOFTWARE);
	}

	/* Fiber entry function (see above). */
	memset(&fpm_fiber_entry_fn, 0, sizeof(fpm_fiber_entry_fn));
	fpm_fiber_entry_fn.type = ZEND_INTERNAL_FUNCTION;
	fpm_fiber_entry_fn.function_name = zend_string_init_interned("fpmng_fiber_request", sizeof("fpmng_fiber_request") - 1, 1);
	fpm_fiber_entry_fn.handler = fpm_fiber_entry_handler;
	ZEND_MAP_PTR_INIT(fpm_fiber_entry_fn.run_time_cache, NULL);

	if (fpm_coop_container_start(wp->config->name) < 0) {
		exit(FPM_EXIT_SOFTWARE);
	}

	/* Transports: we are after MINIT (fpm_main.c: startup() before fpm_run()),
	 * that is, after ext/openssl, which overrides "tcp" in its MINIT. */
	fpm_pool_fiber_xport_install();

	/* SPIKE (docs/flock-streams-spike-report.md): intercept
	 * PHP_STREAM_OPTION_LOCKING for ordinary files so that flock()/
	 * file_put_contents(..., LOCK_EX) on a file held by ANOTHER Fiber in this
	 * process suspends on an in-memory queue instead of blocking the entire event
	 * loop in the kernel (see docs/flock-fiber-deadlock-report.md,
	 * spike/flock-fiber). Install it here, not in MINIT: the same rule as
	 * xport_install above. */
	fpm_pool_fiber_flock_install();

	/* Sleep family (sleep/usleep/time_nanosleep): see fpm_pool_fiber_sleep.h.
	 * The order relative to xport_install does not matter here (different
	 * functions in the function table), but keep the same location in child_main
	 * because this is the only installation point specific to the fiber pool type. */
	fpm_pool_fiber_sleep_install();

	fpm_fiber_ev_accept = event_new(fpm_fiber_base, fpm_fiber_listen_fd, EV_READ | EV_PERSIST, fpm_fiber_accept_cb, NULL);
	fpm_fiber_ev_tick = event_new(fpm_fiber_base, -1, EV_PERSIST, fpm_fiber_tick_cb, NULL);
	event_add(fpm_fiber_ev_accept, NULL);
	event_add(fpm_fiber_ev_tick, &tick);

	/* fiber.revalidate_freq: compile hook (after container_start, which checks
	 * zend_compile_file) plus a sweep timer. 0 = none of this exists. */
	if (wp->config->fiber_revalidate_freq > 0) {
		struct timeval every = { wp->config->fiber_revalidate_freq, 0 };

		fpm_coop_reval_start(wp->config->fiber_revalidate_freq);
		fpm_fiber_ev_reval = event_new(fpm_fiber_base, -1, EV_PERSIST, fpm_fiber_reval_cb, NULL);
		event_add(fpm_fiber_ev_reval, &every);
		zlog(ZLOG_NOTICE, "[pool %s] fiber: revalidate_freq = %ds, worker replaces itself when an included file changes on disk",
			wp->config->name, wp->config->fiber_revalidate_freq);
	}

	zlog(ZLOG_NOTICE, "[pool %s] fiber: child %d ready, PHP %s, libevent %s (%s), one process, N requests in flight",
		wp->config->name, (int) getpid(), PHP_VERSION, event_get_version(), event_base_get_method(fpm_fiber_base));

	event_base_dispatch(fpm_fiber_base);

	if (fpm_fiber_draining && !fcgi_in_shutdown()) {
		unsigned files, sweeps, stats;

		fpm_coop_reval_stats(&files, &sweeps, &stats);
		zlog(ZLOG_NOTICE, "[pool %s] fiber: exiting for fresh code, %u request(s) in flight, %u file(s) tracked, %u sweep(s), %u stat() total",
			wp->config->name, fpm_coop_in_flight(), files, sweeps, stats);
	} else {
		zlog(ZLOG_NOTICE, "[pool %s] fiber: shutdown requested, %u request(s) in flight abandoned",
			wp->config->name, fpm_coop_in_flight());
	}

	fpm_stdio_flush_child();
	exit(FPM_EXIT_OK);
}
/* }}} */

#endif /* ZTS */
