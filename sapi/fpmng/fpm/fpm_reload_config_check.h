/* fpm-ng: issue #640 -- the configuration gate in front of a SIGUSR2 reload.
 *
 * A reload is a full execvp() of the master (fpm_process_ctl.c,
 * fpm_pctl_exec()): the next generation parses the configuration from scratch,
 * and if that parse fails the next master exits and the whole service is down.
 * nginx keeps the old configuration when the new one is invalid; this is that
 * behaviour, one fork away, and it is the reason the gate has to run BEFORE the
 * reload sequence rather than in fpm_pctl_exec(): by the time fpm_pctl_exec()
 * is reached every child has already been signalled and has exited
 * (fpm_pctl_action_next()), so a refusal there would leave nothing serving.
 *
 * The check is the configuration test this binary already has -- `-t`, which
 * fpm_conf_init_main() runs and then refuses to continue with
 * (fpm_conf.c:2743-2755, fpm_init()'s FPM_INIT_EXIT_OK branch at fpm.c:93) --
 * asked with the arguments THIS master was started with, which are the
 * arguments fpm_pctl_exec() is about to re-exec. Not a second, cheaper
 * approximation of the parse: the same binary, the same argv, the same php.ini.
 *
 * It runs in a child that execs the binary rather than in a fork of the master
 * that parses the file in place, so it shares nothing with the running
 * generation that could confuse it or be disturbed by it:
 *
 *   - no worker is forked, no listening socket is bound, no pid file is
 *     written: fpm_init() returns from inside fpm_conf_init_main(), so the
 *     scoreboard, the metrics region, the signal handlers and
 *     fpm_conf_write_pid() are never reached at all (fpm.c:81-106);
 *   - the close-on-exec memfds of a selective reload (fpm_reload_shm.h) are
 *     closed by the exec itself, and nothing can write the scoreboard or the
 *     counters because there are none;
 *   - the listening sockets and the log descriptors the child inherits by fork
 *     it only ever writes to the log (below), never accepts on, never rebinds.
 *
 * Where the operator sees the child's diagnostics: the child's zlog writes to
 * the `error_log` the configuration it just parsed names, and anything that
 * goes to stderr goes to the descriptor the master tied to that same file
 * (`fpm_stdio_init_final()` at fpm_stdio.c:69 ->
 * `fpm_stdio_redirect_stderr_to_error_log()` at fpm_stdio.c:115). Either way it
 * is the log the operator already reads, and the ERROR line this gate adds says
 * which reload it belongs to.
 *
 * What `-t` does NOT catch is a documented list, not an accident:
 * docs/reload.md, "What `-t` does not catch". The TOCTOU window between this
 * check and the execvp() it guards is in that list too, and is out of reach by
 * construction (the issue says so).
 */

#ifndef FPM_RELOAD_CONFIG_CHECK_H
#define FPM_RELOAD_CONFIG_CHECK_H 1

/* Returns 1 when the reload may go ahead, 0 when it must not.
 *
 * `argc`/`argv` are the arguments this master was saved at startup
 * (fpm_process_ctl.c's own copy, which is the one fpm_pctl_exec() re-execs --
 * NOT fpm_globals.argv, which fpm_env.c rewrites in place on Linux).
 *
 * A refusal (return 0) happens when the check did not pass. The caller leaves
 * its state untouched, so every pool keeps serving the configuration it was
 * started with, and the next SIGUSR2 tries again. An ERROR is logged in both
 * refusal cases:
 *   - the check ran to completion and said the configuration is not loadable:
 *     the ERROR names the file, the exit status and the reason;
 *   - the check could not exec the binary (execvp() failed, for example because
 *     the binary was removed). Issue #661: a reload that goes ahead drains the
 *     running generation before the next master's execvp() runs, so a failed
 *     exec would leave the service down. The running generation keeps its
 *     listeners and keeps serving. Before #661 this case logged a WARNING and
 *     returned 1 (#690).
 * Anything else -- a fork() or a pipe() that failed, a check killed by a
 * signal -- logs a WARNING and returns 1: the gate exists to keep a *broken
 * configuration* from taking the service down, and turning a transient fork
 * failure into "this master can never reload again" would do more damage than
 * the gate prevents. The failed-execvp() path of fpm_pctl_exec() (#690) is now
 * reached only when the binary disappears between this check and the exec.
 * test fpmng-reload-selective-failed-exec.phpt checks the refusal.
 *
 * Reached from two places, both through fpm_pctl(): the operator's SIGUSR2
 * (fpm_events.c) and the emergency restart after enough children died at once
 * (fpm_children.c). The second one is gated too, on purpose -- a crash loop
 * wants the configuration re-read, and a configuration that does not load is
 * not a better answer than the one that is already loaded. */
int fpm_reload_config_check(int argc, const char *const *argv);

#endif
