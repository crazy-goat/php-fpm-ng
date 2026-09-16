/* fpm-ng: shared "run one PHP script in this process, outside of any FastCGI
 * request" helper. Used by pool.type = supervisor (looped, in the same
 * process) and pool.type = cron (once, then the process exits). Extracted
 * from fpm_pool_supervisor.c when cron needed the exact same thing.
 */

#ifndef FPM_POOL_SCRIPT_H
#define FPM_POOL_SCRIPT_H 1

/* Overrides a handful of sapi_module callbacks for the lifetime of THIS
 * process. Safe only because a process that calls this never returns to the
 * FastCGI accept loop — see the long comment in fpm_pool_script.c. Call once,
 * before the first fpm_pool_script_run(). */
void fpm_pool_script_install_sapi_overrides(void);

/* Runs script_path once: php_request_startup / php_fopen_primary_script /
 * php_execute_script / php_request_shutdown, without any SG(request_info)
 * filled in from FastCGI. pool_name is used only for log messages.
 *
 * Registers STDIN, STDOUT and STDERR for the script before running it, as the
 * CLI SAPI does — see fpm_std_streams_register() for where those three
 * descriptors point in such a child, and why the streams are request-scoped
 * here and process-scoped in CLI.
 *
 * php_request_startup() and php_request_shutdown() REPLACE several signals'
 * dispositions with Zend's own handler on every call (ZEND_SIGNALS, zend_sigs[]
 * in Zend/zend_signal.c: TIMEOUT_SIG, SIGHUP, SIGINT, SIGQUIT, SIGTERM,
 * SIGUSR1, SIGUSR2 -- NOT only SIGTERM) — this function saves and restores the
 * disposition of SIGTERM, always, and of stop_signal as well when it names one
 * of the OTHER signals Zend touches (QUIT/USR1/USR2), so the caller's own
 * handler (installed before its loop) stays in effect between iterations
 * rather than silently reverting to the process default the moment Zend's
 * per-request signal snapshot/restore machinery resets it (see the long
 * comment next to term_before in fpm_pool_script.c for exactly how that
 * reset happens on the SECOND iteration onward, and issue #324's
 * supervisor.stop_signal / issue #325's cron.stop_signal for the callers
 * that need this).
 *
 * stop_signal is the signal name the caller cares about beyond SIGTERM itself
 * -- pass SIGTERM here too when there is no other one (a supervisor or cron
 * pool left at the default stop_signal = TERM) and this is a no-op repeat of
 * the SIGTERM handling, not a second, different save/restore.
 *
 * Returns EG(exit_status) of the script (0 = normal end / exit(0), != 0 =
 * exit($n) or a fatal error) — this is the script's own exit status, not a
 * process exit code (the process does not necessarily end after this call).
 */
int fpm_pool_script_run(const char *pool_name, const char *script_path, int stop_signal);

#endif
