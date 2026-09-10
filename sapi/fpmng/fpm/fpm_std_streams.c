/* fpm-ng: see fpm_std_streams.h. */

#include "fpm_config.h"

#include <string.h>

#include "php.h"
#include "php_streams.h"
#include "zend_constants.h"

#include "fpm_std_streams.h"
#include "zlog.h"

/* Nothing else in an FPM process registers these three: they belong to the CLI
 * SAPI, so the first thing an author of a supervised script or of a worker
 * script reaches for to write a line — fwrite(STDERR, ...) — was a fatal
 * "Undefined constant". The two process types that call this are the ones
 * where a script is exactly what CLI runs, so they get exactly what CLI gives
 * it.
 *
 * Where the three descriptors actually point (fpm_stdio.c): stdin is the
 * /dev/null fpm_stdio_init_main() installs, so STDIN is a valid handle that
 * returns EOF at once; stdout and stderr are the pipes to the master when the
 * pool sets catch_workers_output, and /dev/null when it does not. Both are
 * documented in docs/cron.md — a script that needs its output kept uses
 * error_log() (issue #124), not STDOUT.
 *
 * Deliberately NOT a copy of CLI's cli_register_file_handles(): that one sets
 * PHP_STREAM_FLAG_NO_RSCR_DTOR_CLOSE to keep the handles alive for extensions
 * writing to stderr during MSHUTDOWN, which it can afford because it runs one
 * request and exits. A supervisor or cron process runs a request per iteration
 * (supervisor restart = always is measured at 12086 runs/s in issue #122), and
 * outside CLI php://std* dup()s the descriptor
 * (ext/standard/php_fopen_wrapper.c:266ff), so preserving the handle would
 * leak one fd per run and hit the fd limit within seconds. Plain
 * request-scoped streams instead: php_request_shutdown() closes the dup, and
 * the constants — non-persistent — go with it, which is also why a
 * script-running pool has to call this again for every iteration.
 *
 * A worker (pool.executor = worker, issue #73) calls it exactly once instead,
 * and that is the same rule rather than an exception to it: one
 * php_request_startup() covers the whole worker, so "request-scoped" and
 * "process-scoped" are the same scope there and the dup happens once.
 */
void fpm_std_streams_register(const char *pool_name)
{
	static const struct {
		const char *constant;
		const char *path;
		const char *mode;
	} handles[] = {
		{ "STDIN",  "php://stdin",  "rb" },
		{ "STDOUT", "php://stdout", "wb" },
		{ "STDERR", "php://stderr", "wb" },
	};
	size_t i;

	for (i = 0; i < sizeof(handles) / sizeof(handles[0]); i++) {
		php_stream *stream;
		zend_constant c;

		/* 0 options: no REPORT_ERRORS. A failure here is this pool's problem,
		 * not the script's, so it is reported once through the pool's own log
		 * channel rather than as a PHP warning attributed to the script. */
		stream = php_stream_open_wrapper_ex((char *) handles[i].path,
			(char *) handles[i].mode, 0, NULL, NULL);
		if (!stream) {
			zlog(ZLOG_WARNING, "[pool %s] cannot open %s; the %s constant will "
				"not exist for this run", pool_name, handles[i].path, handles[i].constant);
			continue;
		}

		/* One missing handle does not cost the script the other two — unlike
		 * CLI, which registers all three or none. */
		php_stream_to_zval(stream, &c.value);
		ZEND_CONSTANT_SET_FLAGS(&c, 0, 0);
		c.name = zend_string_init_interned(handles[i].constant,
			strlen(handles[i].constant), 0);
		zend_register_constant(&c);
	}
}
