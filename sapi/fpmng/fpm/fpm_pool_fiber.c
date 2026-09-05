/* fpm-ng: pool.type = fiber — scheduler na libevent + fibry silnika.
 * Patrz fpm_pool_fiber.h i docs/NOTES.md 3u.
 *
 * Kto przelacza: WYLACZNIE ten plik, z kontekstu glownego (petla libevent).
 * Dlatego nie potrzebujemy switch-handlerow forka — stan requestu wchodzi do
 * globali tuz przed zend_fiber_start/resume i wychodzi tuz po ich powrocie
 * (fpm_coop_req_enter/leave). Fiber requestu zawiesza sie tylko przez
 * fpm_pool_fiber_wait_fd() (z fpm_pool_fiber_xport.c) i zawsze wraca tu.
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
#include "fpm_pool_fiber.h"
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
	zlog(ZLOG_ALERT, "[pool %s] pool.type = fiber: ZTS build, this should have been rejected by validate()",
		wp->config->name);
	exit(FPM_EXIT_SOFTWARE);
}
/* }}} */

int fpm_pool_fiber_can_wait(void) { return 0; }
int fpm_pool_fiber_wait_fd(int fd, short events, struct timeval *timeout) { (void) fd; (void) events; (void) timeout; return -1; }

#else /* !ZTS */

/* Request w locie z punktu widzenia schedulera. */
struct fpm_fiber_req_s {
	struct fpm_coop_req_s *ctx;
	zend_fiber *fiber;
	struct event *ev;			/* jedno zdarzenie I/O na request, przypinane per czekanie */
	short wait_result;			/* co obudzilo: EV_READ/EV_WRITE/EV_TIMEOUT */
	bool waiting;
};

/* Polaczenie keep-alive miedzy requestami: czekamy na kolejny request. */
struct fpm_fiber_kept_s {
	fcgi_request *req;
	int fd;
	struct event *ev;
};

static struct event_base *fpm_fiber_base;
static struct event *fpm_fiber_ev_accept;
static struct event *fpm_fiber_ev_tick;
static int fpm_fiber_listen_fd = -1;

/* Request, ktorego fiber jest wlasnie na procesorze (NULL w petli zdarzen). */
static struct fpm_fiber_req_s *fpm_fiber_current;
/* Request, ktorego fiber wlasnie startuje (przekazanie ctx do funkcji wejsciowej). */
static struct fpm_fiber_req_s *fpm_fiber_starting;

/* Funkcja wewnetrzna bedaca "callable" fibera. Fiber to obiekt PHP i chce
 * fci/fci_cache; z fci_cache.function_handler ustawionym na gotowa
 * zend_function zend_call_function nie szuka nic po nazwie. Zero argumentow,
 * zero arg_info, bez rejestracji w tablicy funkcji — niewidoczna z PHP. */
static void fpm_fiber_entry_handler(INTERNAL_FUNCTION_PARAMETERS);
static zend_internal_function fpm_fiber_entry_fn;

/* Keep-alive: idle deadline na polaczeniu bez requestu. */
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

/* Po kazdym powrocie z fibera: skonczyl? — posprzataj i zajmij sie polaczeniem. */
static void fpm_fiber_after_switch(struct fpm_fiber_req_s *fr) /* {{{ */
{
	fcgi_request *req;
	int fd;

	if (fr->fiber->context.status != ZEND_FIBER_STATUS_DEAD) {
		if (!fr->waiting) {
			/* Fiber zawiesil sie NIE przez nasze wait_fd (np. Fiber::suspend()
			 * z kodu uzytkownika w glownym fiberze requestu). Nikt go nie
			 * obudzi — traktujemy jak koniec requestu z bledem. */
			zlog(ZLOG_WARNING, "[pool %s] fiber: request #%u suspended outside the scheduler (Fiber::suspend() in the request's main fiber?); dropping it",
				fpm_coop_pool_name(), fr->ctx->id);
			fcgi_finish_request(fr->ctx->req, 1);
			fr->ctx->req = NULL;
			/* Obiekt fibera zwolnimy przy wyjsciu procesu — jego zniszczenie
			 * w stanie SUSPENDED wznawia go z graceful exit w ZLYM stanie globali. */
			return;
		}
		return;
	}

	fd = fr->ctx->fd;
	req = fpm_coop_req_free(fr->ctx);
	event_free(fr->ev);
	OBJ_RELEASE(&fr->fiber->std);
	efree(fr);

	if (fcgi_is_closed(req)) {
		fcgi_destroy_request(req);
		return;
	}

	/* Klient chce keep-alive (bramka HTTP tak robi): czekamy na nastepny
	 * request na tym fd w petli zdarzen, nie blokujac. */
	{
		struct fpm_fiber_kept_s *kept = emalloc(sizeof(*kept));

		kept->req = req;
		kept->fd = fd;
		kept->ev = event_new(fpm_fiber_base, fd, EV_READ, fpm_fiber_kept_cb, kept);
		event_add(kept->ev, &fpm_fiber_keep_idle);
	}
}
/* }}} */

static void fpm_fiber_kept_cb(evutil_socket_t fd, short what, void *arg) /* {{{ */
{
	struct fpm_fiber_kept_s *kept = arg;
	fcgi_request *req = kept->req;
	int new_fd = kept->fd;

	(void) fd;
	event_free(kept->ev);
	efree(kept);

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

/* Wejscie na procesor: stan requestu -> globale, start/resume, stan <- globale. */
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
		/* Bailout przeciekl z fibera (zend_fiber_switch_to przekazuje go
		 * dalej). fpm_coop_req_run ma wlasne zend_try, wiec to awaria. */
		zlog(ZLOG_ERROR, "[pool %s] fiber: bailout escaped request #%u", fpm_coop_pool_name(), fr->ctx->id);
	} zend_end_try();
	zval_ptr_dtor(&rv);
	if (EG(exception)) {
		/* fiber rzucil (nie powinien: run() sprzata) — nie zostawiamy tego
		 * kontekstowi glownemu, ktory nie ma ramki */
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

/* --- API dla transportu ---------------------------------------------------- */

int fpm_pool_fiber_can_wait(void) /* {{{ */
{
	struct fpm_fiber_req_s *fr = fpm_fiber_current;

	if (!fr || !fpm_fiber_base) {
		return 0;
	}
	/* Zagniezdzony Fiber uzytkownika: zend_fiber_suspend zawiesilby JEGO do
	 * jego wolajacego, nie nasz request do schedulera. Wtedy blokujemy. */
	if (EG(active_fiber) != fr->fiber) {
		return 0;
	}
	/* Destruktory pod GC, ticks, pcntl: silnik zabrania przelaczen. */
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

	if (event_assign(fr->ev, fpm_fiber_base, fd, events, fpm_fiber_io_cb, fr) < 0
		|| event_add(fr->ev, timeout) < 0) {
		return -1;
	}
	fr->wait_result = 0;
	fr->waiting = true;

	/* Do schedulera. Wracamy tu z fpm_fiber_io_cb -> zend_fiber_resume, juz
	 * z podmienionym z powrotem stanem requestu (switch_in robi enter). */
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

/* --- petla zdarzen ------------------------------------------------------------ */

static void fpm_fiber_accept_cb(evutil_socket_t fd, short what, void *arg) /* {{{ */
{
	fcgi_request *req;
	int conn_fd = -1;

	(void) fd; (void) what; (void) arg;

	if (fcgi_in_shutdown()) {
		event_base_loopbreak(fpm_fiber_base);
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

	/* SIGQUIT: fpm_signals.c sig_soft_quit zamyka gniazdo nasluchujace i
	 * wola fcgi_terminate(); zdarzenie na zamknietym fd moze juz nie przyjsc. */
	if (fcgi_in_shutdown()) {
		event_base_loopbreak(fpm_fiber_base);
	}
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

	/* Funkcja wejsciowa fibera (patrz wyzej). */
	memset(&fpm_fiber_entry_fn, 0, sizeof(fpm_fiber_entry_fn));
	fpm_fiber_entry_fn.type = ZEND_INTERNAL_FUNCTION;
	fpm_fiber_entry_fn.function_name = zend_string_init_interned("fpmng_fiber_request", sizeof("fpmng_fiber_request") - 1, 1);
	fpm_fiber_entry_fn.handler = fpm_fiber_entry_handler;
	ZEND_MAP_PTR_INIT(fpm_fiber_entry_fn.run_time_cache, NULL);

	if (fpm_coop_container_start(wp->config->name) < 0) {
		exit(FPM_EXIT_SOFTWARE);
	}

	/* Transporty: jestesmy po MINIT (fpm_main.c: startup() przed fpm_run()),
	 * czyli po ext/openssl, ktore nadpisuje "tcp" w swoim MINIT. */
	fpm_pool_fiber_xport_install();

	fpm_fiber_ev_accept = event_new(fpm_fiber_base, fpm_fiber_listen_fd, EV_READ | EV_PERSIST, fpm_fiber_accept_cb, NULL);
	fpm_fiber_ev_tick = event_new(fpm_fiber_base, -1, EV_PERSIST, fpm_fiber_tick_cb, NULL);
	event_add(fpm_fiber_ev_accept, NULL);
	event_add(fpm_fiber_ev_tick, &tick);

	zlog(ZLOG_NOTICE, "[pool %s] fiber: child %d ready, PHP %s, libevent %s (%s), one process, N requests in flight",
		wp->config->name, (int) getpid(), PHP_VERSION, event_get_version(), event_base_get_method(fpm_fiber_base));

	event_base_dispatch(fpm_fiber_base);

	zlog(ZLOG_NOTICE, "[pool %s] fiber: shutdown requested, %u request(s) in flight abandoned",
		wp->config->name, fpm_coop_in_flight());

	fpm_stdio_flush_child();
	exit(FPM_EXIT_OK);
}
/* }}} */

#endif /* ZTS */
