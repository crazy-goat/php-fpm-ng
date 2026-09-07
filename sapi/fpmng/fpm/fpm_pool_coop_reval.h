/* fpm-ng: multi-request executor (Fiber) — detect changes to LOADED files on
 * disk, through the fiber.revalidate_freq directive.
 *
 * Problem: the process loads the application once (function and class tables
 * are process-wide, and with FPMNG_SHARED_INCLUDES=1 the loaded-file list is
 * too), while the entry script is read from disk on every request. After files
 * are replaced without a reload, a new index.php runs on an old bootstrap —
 * silently, without an error (docs/fiber_errors.md).
 *
 * Solution: remember each file's mtime/size/inode when the engine compiles it
 * (zend_compile_file hook — exactly when opened_path enters EG(included_files):
 * compile_filename in zend_language_scanner.l and zend_include_or_eval in
 * zend_execute.c). The pool type calls sweep() periodically (every
 * fiber.revalidate_freq seconds, NOT per request), and when a file changes it
 * gracefully ends the worker; the master replaces it.
 *
 * The entry script (file_handle->primary_script) is skipped: it is executed
 * directly and read from disk on every request, so changing it does not require
 * replacing the process. Request-path cost: one table lookup per compilation
 * and one stat() per file seen for the FIRST TIME in the process.
 */

#ifndef FPM_POOL_COOP_REVAL_H
#define FPM_POOL_COOP_REVAL_H 1

#include <stdbool.h>
#include <stddef.h>

/* Enable tracking and install the compile hook. freq <= 0 = do nothing.
 * Call in the child, after fpm_coop_container_start(), before the first request. */
void fpm_coop_reval_start(int freq_seconds);

bool fpm_coop_reval_enabled(void);

/* One sweep: stat() every remembered file. 1 = one changed (path points into
 * our table, why describes the change), 0 = unchanged. A file that cannot be
 * checked (stat() error, including ENOENT) counts as changed — a deploy through
 * rename/rsync passes through that state, and a fresh worker is the intended result. */
int fpm_coop_reval_sweep(const char **path, char *why, size_t why_len);

/* Log statistics: number of tracked files, sweeps, and stat() calls since
 * process start (including registration). */
void fpm_coop_reval_stats(unsigned *files, unsigned *sweeps, unsigned *stat_calls);

#endif
