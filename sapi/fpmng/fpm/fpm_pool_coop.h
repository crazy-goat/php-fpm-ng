/* fpm-ng: shared core for pool types handling MANY FastCGI requests in ONE
 * PHP process. It is currently used by pool.executor = fiber.
 *
 * Model: the child performs ONE php_request_startup() (the "request container"),
 * and every FastCGI request gets its own state (SG, output buffers,
 * EG(symbol_table), EG(included_files), superglobals, error handlers), which the
 * pool type swaps into the engine globals immediately before giving that
 * request the processor (enter) and immediately after it leaves (leave). WHO
 * switches and WHEN is the pool type's concern (Fiber has its own libevent
 * scheduler). The core knows nothing about fibers or coroutines.
 *
 * What is SHARED between in-flight requests and what this core does NOT isolate
 * (deliberate limitation, see docs/NOTES.md 3t and 3u): function and class
 * tables, memory_limit, max_execution_time, class statics,
 * register_shutdown_function, extension RINIT/RSHUTDOWN, OPcache, signal
 * handlers (pcntl_signal: PCNTL_G(php_signal_table) and SIGG(handlers) are one
 * process-wide table), SIGALRM timer (pcntl_alarm), fork/exec.
 * The VALUE of ini entries (ini_get/ini_set) IS isolated per request — see
 * fpm_pool_coop_ini.[ch] — but only at ini_entry->value level; process globals
 * also updated by on_modify for some entries (for example core_globals.precision)
 * are NOT isolated (exception: ext/session, see fpm_pool_coop_session.c) — see
 * the rationale in fpm_pool_coop_ini.c. This explains what validate() REJECTS
 * (OPcache enabled, max_execution_time != 0) and what the container BLOCKS with
 * zend_disable_functions (process-wide pcntl functions) — see fpm_pool_coop.c
 * and docs/fiber_errors.md.
 *
 * Reason for a separate file: removing either of the two types should mean
 * removing one file and one registry line, without touching the other type's
 * code (decision in NOTES 3s).
 */

#ifndef FPM_POOL_COOP_H
#define FPM_POOL_COOP_H 1

#include <stdbool.h>

#include "php.h"
#include "SAPI.h"
#include "php_output.h"
#include "php_variables.h"
#include "zend_stack.h"
#include "fastcgi.h"
/* zend_ps_globals — ONLY for the struct size (sizeof below). Including this
 * header creates no linker dependency by itself (exactly as it does, without
 * any guard, in ext/standard/basic_functions.c, which is always compiled) — a
 * dependency would arise only from using ZEND_EXTERN_MODULE_GLOBALS(ps) or
 * referring to ps_globals by name, and no fpm_pool_coop*.c does that. See
 * fpm_pool_coop_session.c. */
#include "ext/session/php_session.h"

/* State of one in-flight request when it is NOT on the processor. When it is,
 * the same state lives in engine globals and this structure is stale. */
struct fpm_coop_req_s {
	fcgi_request *req;
	int fd;					/* connection descriptor (fcgi_request is opaque) */
	unsigned id;

	sapi_globals_struct sg;			/* complete SG */
	zend_output_globals og;			/* ob_* stack, output flags */

	/* The following exist only from the start of fpm_coop_req_run() to its end
	 * (live). */
	bool live;
	HashTable symbol_table;			/* $GLOBALS for this request */
	HashTable included_files;
	zval http_globals[NUM_TRACK_VARS];	/* PG(http_globals): $_GET, $_POST, ... */
	zval user_error_handler;
	zval user_exception_handler;
	int user_error_handler_error_reporting;
	zend_stack user_error_handlers_error_reporting;
	zend_stack user_error_handlers;
	zend_stack user_exception_handlers;

	/* Snapshot of ps_globals (ext/session) for this request when it is NOT on
	 * the processor — see fpm_pool_coop_session.[ch]. Independent of "live":
	 * zero bytes before the first fpm_coop_session_req_save() are harmless (they
	 * are never read until fpm_coop_session_request_startup() writes the base
	 * state to live globals). The size is always counted, even when session is
	 * not loaded — the cost is a few hundred bytes per request context, with no
	 * allocation. */
	unsigned char session_globals[sizeof(zend_ps_globals)];

	/* INI entries that THIS request changed while it was NOT live — see
	 * fpm_pool_coop_ini.[ch]. Both are NULL until the request suspends with a
	 * non-empty EG(modified_ini_directives) — so for the VAST MAJORITY of Fiber
	 * switches (without ini_set/set_time_limit/...) the cost is zero. */
	HashTable *ini_mods;			/* name -> zend_ini_entry* (same table as EG(modified_ini_directives)) */
	HashTable *ini_values;			/* name -> zend_string*: this request's OWN value */

	/* Values stashed from fiber.isolate_statics (fpm_pool_coop_statics.c)
	 * while this request is not live. Array of zval, one per configured item,
	 * NULL until the first fpm_coop_statics_req_leave() with a non-empty item
	 * list allocates it (empty list, the default: never). */
	void *statics;

	void *type_data;			/* pool-type private data (Fiber: zend_fiber + event) */
};

/* The only php_request_startup() in the process lifetime + capture of the base
 * state + replacement of SAPI hooks (ub_write/flush without server_context,
 * read_post without the static request_body_fd from fpm_main.c, FastCGI
 * environment import). Call once in child_main, BEFORE accepting the first
 * connection. Returns 0 or -1. */
int fpm_coop_container_start(const char *pool_name);

/* Pool name for logs. */
const char *fpm_coop_pool_name(void);

/* accept() + read FastCGI headers (blocking, but called only when the listening
 * socket is ready). NULL: nothing to handle (EAGAIN, error, shutdown). Give the
 * returned request to fpm_coop_req_new(). */
fcgi_request *fpm_coop_accept(int listen_fd, int *fd_out);

/* As above, but for a persistent connection after the previous request: read
 * the next request from the same fd. If the client closed the connection,
 * destroy req and return NULL. Call only when fd is readable — otherwise it
 * blocks. */
fcgi_request *fpm_coop_accept_kept(fcgi_request *req, int *fd_out);

/* New request context (nothing executed yet). */
struct fpm_coop_req_s *fpm_coop_req_new(fcgi_request *req, int fd);

/* Request state -> engine globals. Call immediately BEFORE giving it the
 * processor. */
void fpm_coop_req_enter(struct fpm_coop_req_s *ctx);

/* Engine globals -> request state, base (container) state -> globals. Call
 * immediately AFTER the request leaves the processor (suspension or end). */
void fpm_coop_req_leave(struct fpm_coop_req_s *ctx);

/* Complete request handling: SAPI activation, fresh tables, script, headers,
 * flush, fcgi_finish_request, cleanup. Call IN THE REQUEST CONTEXT (on its
 * stack, after enter). It may yield the processor in the middle (I/O) — the
 * pool type performs leave/enter around every switch. On return the request is
 * finished, while globals are still "entered" (the type calls leave). */
void fpm_coop_req_run(struct fpm_coop_req_s *ctx);

/* Free the context (after leave). Returns fcgi_request: !fcgi_is_closed(req)
 * when the client wants keep-alive and we must wait for ctx->fd; otherwise
 * destroy it with fcgi_destroy_request. */
fcgi_request *fpm_coop_req_free(struct fpm_coop_req_s *ctx);

/* In-flight requests (log statistic). */
unsigned fpm_coop_in_flight(void);

/* Shared validation for both types: pm = static, NTS. Returns 0 or -1. */
struct fpm_worker_pool_s;
int fpm_coop_validate(struct fpm_worker_pool_s *wp, const char *type_name);

/* Shared rejected-directive list (the scoreboard does not see in-flight
 * requests). */
extern const char *const fpm_coop_rejects[];

#endif
