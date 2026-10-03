/* fpm-ng: a per-pool output log for pool.type = cron/supervisor, so a script
 * that wants its stdout/stderr captured does not have to pay for
 * catch_workers_output (issue #328).
 *
 * docs/cron.md documents catch_workers_output's cost explicitly: every line a
 * worker writes travels through the master's reader thread, measured at 52 MB
 * in 15 s for a supervisor pool restarting a short script in a loop (see the
 * comment in fpmng-supervisor-restart.phpt) -- expensive enough that the same
 * page tells a script that wants its output kept to call error_log() instead
 * (issue #124). That leaves a script whose stdout/stderr is not its own to
 * change (a third-party command-line tool run as a cron job, say) with only
 * the shared pipe as an alternative. cron.output_log / supervisor.output_log
 * are that alternative: a file path this pool's stdout and stderr are
 * redirected to directly, once, bypassing catch_workers_output and its
 * reader thread entirely -- whether or not that directive is also set on the
 * same pool.
 */

#ifndef FPM_POOL_OUTPUT_LOG_H
#define FPM_POOL_OUTPUT_LOG_H 1

/* Opens path (O_CREAT | O_WRONLY | O_APPEND, 0644) and dup2()s it onto both
 * STDOUT_FILENO and STDERR_FILENO, replacing whatever fpm_stdio_init_child()
 * already put there for this child (the catch_workers_output pipe, or
 * /dev/null -- see fpm_std_streams.h). path may be NULL or empty, meaning the
 * directive is not set: a no-op, leaving stdout/stderr exactly as
 * fpm_stdio_init_child() left them.
 *
 * Call once per child process, before the first fpm_pool_script_run() --
 * see fpm_pool_cron_child_main() and fpm_pool_supervisor_child_main(). Not
 * per iteration: this is a process-level file descriptor redirection, not
 * request-scoped state, so re-running it on every supervisor loop iteration
 * would only add an open()/dup2()/close() nobody asked for -- the same file
 * remains open (O_APPEND) across iterations and across the whole life of the
 * process, exactly like a shell's own `>>` redirection would.
 *
 * Returns 0 on success (including the no-op case), -1 if path was given but
 * could not be opened or dup2()'d -- logged via zlog(); both current callers
 * treat that as non-fatal (the pool keeps running with whatever stdio
 * fpm_stdio_init_child() already gave it, the same tolerance cron.log's own
 * open() failure gets).
 */
int fpm_pool_output_log_redirect(const char *pool_name, const char *path);

#endif
