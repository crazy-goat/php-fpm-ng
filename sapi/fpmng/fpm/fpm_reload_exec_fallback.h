/* fpm-ng: issue #925 -- a second try for the exec of a reload.
 *
 * fpm_pctl_exec() runs after every child has exited, the pool list is freed
 * and the listeners are marked for the next master, so the old master cannot
 * go back to serving: there is nothing left to serve with. The only way to
 * keep the service up after a failed execvp(saved_argv[0]) is to start the
 * next generation some other way. That is what the config check cannot see:
 * it execs the same path, so a binary that disappears between the check and
 * the exec (a package upgrade that unlinks the file first) passes the check
 * and then fails here.
 *
 * On Linux /proc/self/exe names the running image even when its file is
 * unlinked, so the exec still works. The next master runs the binary this
 * master runs (not the one that replaced it at the path), with the same argv
 * and the same configuration, and it takes over the listeners as in a normal
 * reload. Its own saved argv[0] is still the missing path, so the next SIGUSR2
 * is refused by the configuration check (#661) until the binary is back.
 *
 * Rejected: restarting the workers inside the old master. The cleanups of
 * FPM_CLEANUP_PARENT_EXEC have freed the pools, and re-running the start-up
 * from there would be a second implementation of fpm_run(). */

#ifndef FPM_RELOAD_EXEC_FALLBACK_H
#define FPM_RELOAD_EXEC_FALLBACK_H 1

/* Returns only when the exec failed too (or the platform has no
 * /proc/self/exe). errno is the failure of that last attempt. */
void fpm_reload_exec_fallback(char *const *argv);

#endif
