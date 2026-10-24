/* fpm-ng: a failure-injection seam for the test suite (issue #832).

   WHY THIS EXISTS. Some error paths cannot be reached by any configuration.
   The #690 discard in fpm_pctl_exit() runs when type->init_main() fails after
   fpm_init() returned OK, and every init_main() failure is a resource failure
   (fpm_shm_alloc(), calloc(), fpm_event_add(), fcntl()): config validation
   refuses a bad pool earlier. Without a way to make the call fail, the path
   has no test, and a regression in it is a stray worker holding a port.
   #890 (a failing fork() in the survivor watchdog) and #962 (a failing
   malloc()/setenv() in fpm_reload_selective_spare_pool()) are the same kind
   of path and use the same mechanism. The #890 site runs inside a signal
   handler (fpm_pool_supervisor_sigterm() calls fpm_pool_watchdog_arm(),
   fpm_pool_watchdog.c:67), so the hit path must be async-signal-safe.

   HOW. A call site asks FPM_FAULT_POINT("name"); it is true when the
   environment variable FPMNG_FAULT selects that point:

       FPMNG_FAULT=<point>[:ifenv=<VAR>][,<point>...]

   `ifenv=VAR` arms the point only in a process whose environment has VAR set.
   It is what lets one test fail the NEW master and not the first one: the
   exec'd master inherits the environment of the old one, so the variable
   alone cannot tell the two apart, but FPMNG_SELECTIVE_RELOAD_SURVIVORS is
   set only in a master that a selective reload started. The variable and every
   `ifenv=` are read once, by a constructor before main(), into a fixed table
   (16 points, names under 64 characters; an entry that does not fit is
   dropped). Later changes of the environment have no effect. A point that is hit
   fires every time; the call sites so far sit at most once per pool in the
   master, and the first failure ends the process.

   Point names used so far:
     pool_type_init_main   fpm_run(): type->init_main() of a pool fails
     pool_watchdog_fork    fpm_pool_watchdog_arm(): fork() of a watchdog fails (issue #890)

   WHY A COMPILE FLAG AND NOT JUST THE ENVIRONMENT VARIABLE. The same reason
   as fpm_debug_clock.h: with --enable-fpmng-fault-inject off -- the default,
   and what every shipped package is built with -- none of this is in the
   binary. FPM_FAULT_POINT() is the constant 0, the compiler drops the branch,
   and there is no variable that could make a production master fail on
   purpose. The packages assert the absence of fpm_fault_inject_hit.

   HOW A TEST DETECTS THE BUILD. The binary carries the literal of the
   variable name only when the flag is on, so fpmng_skip_if_no_fault_inject()
   (sapi/fpmng/tests/fpmng-skipif.inc) looks for it. The package gate runs the
   owned suite against package binaries, which are built without the flag, so
   such a test must SKIP there. */

#ifndef FPM_FAULT_INJECT_H
#define FPM_FAULT_INJECT_H 1

#ifdef HAVE_FPMNG_FAULT_INJECT

/* Returns 1 when the call site `point` must fail now, 0 otherwise.
 *
 * ASYNC-SIGNAL-SAFE: it only reads the static table and calls strcmp(),
 * memcpy(), strlen() and write(2). It does not allocate, call getenv() or take
 * a lock, so a signal handler may call it (the #890 site is one). On a hit it
 * writes one line to stderr, so a log shows that a failure was injected and was
 * not real; it does not use zlog(), which is not safe in a handler. */
int fpm_fault_inject_hit(const char *point);

#define FPM_FAULT_POINT(point) fpm_fault_inject_hit(point)

#else

/* A macro rather than a function, so that a default build carries no symbol
 * and no call: `FPM_FAULT_POINT("x") || expr` folds to `expr`. */
#define FPM_FAULT_POINT(point) 0

#endif

#endif
