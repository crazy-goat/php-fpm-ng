/* fpm-ng: shared script-execution helper. See fpm_pool_script.h. Extracted
 * from fpm_pool_supervisor.c (docs/NOTES.md 3o has the original design
 * writeup) when pool.type = cron needed the exact same "run one script
 * outside of any FastCGI request" plumbing.
 */

#include "fpm_config.h"

#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include "php.h"
#include "php_main.h"
#include "php_variables.h"
#include "SAPI.h"
#include "zend_globals.h"
#include "fopen_wrappers.h"

#include "fpm_pool_script.h"
#include "fpm_std_streams.h"
#include "fpm_acme_challenge.h"
#include "fpm_payload_dist.h"
#include "fpm_pool_type.h"
#include "fpm_worker_pool.h"
#include "fpm_stdio.h"
#include "zlog.h"

/* sapi_module overrides for the lifetime of this process. Safe ONLY because
 * the process that calls this never returns to the FastCGI accept loop
 * (child_main "does not return") — no other code in this process relies on the
 * original cgi_sapi_module pointers. SG(server_context) remains NULL throughout,
 * so the original versions of these callbacks (which unconditionally cast it to
 * fcgi_request*) would segfault. */
static size_t fpm_pool_script_ub_write(const char *str, size_t str_length) /* {{{ */
{
	size_t left = str_length;

	while (left > 0) {
		ssize_t n = write(STDOUT_FILENO, str + (str_length - left), left);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (n == 0) {
			break;
		}
		left -= (size_t) n;
	}
	return str_length - left;
}
/* }}} */

static char *fpm_pool_script_getenv(const char *name, size_t name_len) /* {{{ */
{
	(void) name_len;
	return getenv(name);
}
/* }}} */

static size_t fpm_pool_script_read_post(char *buffer, size_t count_bytes) /* {{{ */
{
	(void) buffer;
	(void) count_bytes;
	return 0;
}
/* }}} */

static char *fpm_pool_script_read_cookies(void) /* {{{ */
{
	return NULL;
}
/* }}} */

static void fpm_pool_script_register_server_variables(zval *track_vars_array) /* {{{ */
{
	/* No HTTP request, so no PHP_SELF or other CGI variables — only the
	 * environment, as in CLI. */
	php_import_environment_variables(track_vars_array);
}
/* }}} */

void fpm_pool_script_install_sapi_overrides(void) /* {{{ */
{
	sapi_module.pre_request_init = NULL;
	sapi_module.ub_write = fpm_pool_script_ub_write;
	sapi_module.getenv = fpm_pool_script_getenv;
	sapi_module.read_post = fpm_pool_script_read_post;
	sapi_module.read_cookies = fpm_pool_script_read_cookies;
	sapi_module.register_server_variables = fpm_pool_script_register_server_variables;
}
/* }}} */

/* The fpmng_acme_challenge_* builtins, for a pool type that declares it may
 * publish challenge answers (fpm_pool_type_s.publishes_acme_challenges). Once
 * per process, not per run: CG(function_table) outlives a request, and a
 * supervisor pool runs its script many times in the same process.
 *
 * A type flag rather than a name comparison, so adding a second publishing
 * type never touches this file -- and so a request-serving type cannot
 * acquire the builtins by accident, which is what keeps the ACME client out
 * of every gateway (issue #48, criterion 7). */
static void fpm_pool_script_register_acme_builtins(const char *pool_name)
{
	static int done = 0;
	struct fpm_worker_pool_s *wp;
	const struct fpm_pool_type_s *type;

	if (done) {
		return;
	}
	done = 1;
	wp = fpm_pool_type_current_pool();
	type = wp ? fpm_pool_type_of(wp) : NULL;
	if (!type || !type->publishes_acme_challenges) {
		return;
	}
	if (0 > fpm_acme_challenge_register_functions()) {
		zlog(ZLOG_ERROR, "[pool %s] cannot register the fpmng_acme_challenge_* functions",
			pool_name);
	}
}

int fpm_pool_script_run(const char *pool_name, const char *script_path, int stop_signal) /* {{{ */
{
	zend_file_handle file_handle;
	int exit_code;
	struct sigaction term_before;
	struct sigaction stop_before;
	/* Whether stop_signal names one of the OTHER signals Zend's zend_sigs[]
	 * touches (Zend/zend_signal.c: TIMEOUT_SIG, SIGHUP, SIGINT, SIGQUIT,
	 * SIGTERM, SIGUSR1, SIGUSR2), and therefore needs its OWN save/restore in
	 * addition to SIGTERM's below. When stop_signal IS SIGTERM (the default for
	 * both current callers), the SIGTERM handling already covers it and this
	 * stays 0 so nothing is saved/restored twice. */
	int stop_is_extra = (stop_signal != SIGTERM);

	/* MEASURED (docs/NOTES.md, "graceful stopping"): php_request_startup()
	 * and php_request_shutdown() REPLACE the process's SIGTERM disposition with
	 * Zend's own handler (ZEND_SIGNALS: zend_sigs[] in Zend/zend_signal.c lists
	 * TIMEOUT_SIG, SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGUSR1 and SIGUSR2 --
	 * NOT only SIGALRM used for max_execution_time) — and they do so on EVERY
	 * call, not only once. The caller (pool.type = supervisor/cron) installs
	 * its OWN SIGTERM handler before the first iteration, BEFORE entering the
	 * loop — without restoring it here, it works only until the FIRST iteration
	 * ends: every subsequent iteration (restart = always/on-failure, many runs
	 * in the same process) gets the default disposition back and SIGTERM kills
	 * the process immediately, with no chance for a graceful stop and without
	 * arming the stop_timeout/cron.timeout watchdog. Save the disposition from
	 * BEFORE php_request_startup() (that is, the one the caller actually
	 * wanted) and restore it immediately after every point where PHP may
	 * replace it — without knowing ANYTHING about the caller (it may be SIG_DFL
	 * if the caller installed nothing of its own; restoring SIG_DFL is then a
	 * no-op, so this is always safe).
	 *
	 * supervisor.stop_signal/cron.stop_signal (issues #324/#325) can also be
	 * QUIT/USR1/USR2, and Zend touches those exactly the same way it touches
	 * SIGTERM (zend_sigs[] above lists all four) — an earlier version of this
	 * function protected ONLY SIGTERM on the theory that Zend left the others
	 * alone, which is false: a pool configured with a non-default stop_signal,
	 * for example, would lose its own handler starting on the SECOND iteration
	 * exactly the way SIGTERM would without the protection below, and a signal
	 * delivered after that point kills the process outright (or dumps core,
	 * for QUIT) with no stop_timeout/cron.timeout grace period at all. Protect
	 * stop_signal the same way, in addition to (never instead of) SIGTERM. */
	sigaction(SIGTERM, NULL, &term_before);
	if (stop_is_extra) {
		sigaction(stop_signal, NULL, &stop_before);
	}

	SG(server_context) = NULL;
	SG(request_info).path_translated = estrdup(script_path);
	SG(request_info).request_method = NULL;
	SG(request_info).query_string = NULL;
	SG(request_info).request_uri = NULL;
	SG(request_info).content_type = NULL;
	SG(request_info).content_length = 0;
	SG(request_info).auth_user = NULL;
	SG(request_info).auth_password = NULL;
	SG(request_info).auth_digest = NULL;
	SG(request_info).proto_num = 1000;
	SG(sapi_headers).http_response_code = 200;

	if (php_request_startup() == FAILURE) {
		zlog(ZLOG_ERROR, "[pool %s] cannot start request for script '%s'", pool_name, script_path);
		efree(SG(request_info).path_translated);
		SG(request_info).path_translated = NULL;
		sigaction(SIGTERM, &term_before, NULL);
		if (stop_is_extra) {
			sigaction(stop_signal, &stop_before, NULL);
		}
		return 255;
	}
	/* See the comment next to term_before at the top of the function. */
	sigaction(SIGTERM, &term_before, NULL);
	if (stop_is_extra) {
		sigaction(stop_signal, &stop_before, NULL);
	}
	/* As with '-v'/phpinfo in fpm_main.c: set this AFTER startup because RINIT
	 * resets no_headers. There is nowhere to send headers, so the send_headers
	 * path is skipped entirely (see sapi_send_headers()). */
	SG(headers_sent) = true;
	SG(request_info).no_headers = 1;

	EG(exit_status) = 0;

	zend_first_try {
		/* Inside the try, not next to the SG() assignments above: this is the
		 * first code in the function that runs PHP (a stream wrapper, a
		 * constant registration), so it is the first that can bail out. */
		fpm_std_streams_register(pool_name);
		fpm_pool_script_register_acme_builtins(pool_name);
		if (fpm_payload_dist_is_path(script_path)) {
			/* Only for a script that asks for it: registering a URL wrapper
			 * reads the payload out of the binary and keeps it in memory for
			 * the life of the process, which a pool running a script from disk
			 * has no reason to pay for. The master already refused the pool if
			 * the file is not there (fpm_pool_cron_validate()), so a failure
			 * here is the payload becoming unreadable after startup -- worth
			 * the message and worth failing the run. */
			const char *why = NULL;

			if (0 > fpm_payload_dist_register(&why)) {
				zlog(ZLOG_ERROR, "[pool %s] cannot use embedded script '%s': %s",
					pool_name, script_path, why);
				EG(exit_status) = 255;
			}
		}

		if (EG(exit_status) == 255) {
			/* The wrapper above failed; there is nothing to open. */
		} else if (fpm_payload_dist_is_path(script_path)) {
			/* php_fopen_primary_script() cannot open this: it resolves the path
			 * through php_resolve_path(), which returns NULL for every scheme
			 * except file:// (fopen_wrappers.c, "Don't resolve paths which
			 * contain protocol"), and a NULL there is a FAILURE before any
			 * wrapper is consulted. Opening the stream directly is the same
			 * pair of calls php_fopen_primary_script() ends with, minus the
			 * resolution step that has nothing to resolve -- an embedded path
			 * is already absolute within the binary, needs no include_path
			 * search and has no realpath. */
			zend_stream_init_filename(&file_handle, script_path);
			file_handle.primary_script = 1;
			if (zend_stream_open(&file_handle) == FAILURE) {
				zlog(ZLOG_ERROR, "[pool %s] cannot open embedded script '%s'",
					pool_name, script_path);
				/* zend_stream_init_filename() emalloc'd the name; nothing else
				 * frees it on this branch. */
				zend_destroy_file_handle(&file_handle);
				EG(exit_status) = 255;
			} else {
				php_execute_script(&file_handle);
				if (!file_handle.in_list) {
					zend_destroy_file_handle(&file_handle);
				}
			}
		} else if (php_fopen_primary_script(&file_handle) == FAILURE) {
			zlog(ZLOG_ERROR, "[pool %s] cannot open script '%s'", pool_name, script_path);
			EG(exit_status) = 255;
		} else {
			php_execute_script(&file_handle);
			if (!file_handle.in_list) {
				zend_destroy_file_handle(&file_handle);
			}
		}
	} zend_catch {
		EG(exit_status) = 255;
	} zend_end_try();

	exit_code = EG(exit_status);

	efree(SG(request_info).path_translated);
	SG(request_info).path_translated = NULL;

	php_request_shutdown((void *) 0);
	/* php_request_shutdown() may replace SIGTERM's (and stop_signal's, when it
	 * is one of the other signals Zend touches) disposition just like
	 * php_request_startup() (see the comment next to term_before) — restore it
	 * once more so the window BETWEEN iterations (backoff, park(), waiting for
	 * the next cron due time) also has the caller's handler active. */
	sigaction(SIGTERM, &term_before, NULL);
	if (stop_is_extra) {
		sigaction(stop_signal, &stop_before, NULL);
	}
	fpm_stdio_flush_child();

	return exit_code;
}
/* }}} */
