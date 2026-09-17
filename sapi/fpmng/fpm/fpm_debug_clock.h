/* fpm-ng: a virtual clock for the test suite (issue #396).

   WHY THIS EXISTS. Four tests in sapi/fpmng/tests spend ~200 of the owned
   suite's ~340 seconds waiting on the real clock, and they cannot be written
   any tighter: fpm_cron_schedule.c parses plain five-field crontab syntax, so
   the shortest possible schedule is "* * * * *", and cron.expect_within is only
   meaningful once the schedule's NEXT occurrence has passed. That is one tick
   (0-60s, because it lands on a minute boundary) plus a further 60s, in wall
   clock, per test run.

   WHY NOT A MOCK IN THE TEST. The clock is read in C, in the master and in the
   cron child, not in PHP -- so mocking DateTime in the test process changes
   nothing about what the process under test believes the time to be.

   WHY NOT A CONSTANT OFFSET. The cron child waits in
   fpm_pool_cron_sleep_until(): "remaining = next - now; sleep(remaining)".
   An offset moves `next` and `now` together, so `remaining` does not change and
   the child sleeps exactly as long. Shortening the wait needs time to run
   FASTER, and needs the blocking sleep divided by the same factor, or the
   clock and the waits disagree.

   WHY NOT libfaketime. It would need no production code at all -- Tester::start()
   passes null as proc_open()'s environment, so an LD_PRELOAD from a --ENV--
   section reaches the master. But build/ci-package-gate.sh runs this same owned
   suite inside the Alpine (apk) flavours at release time, and LD_PRELOAD-based
   faketime does not work on musl. Any test that opted into it would SKIP in the
   one place the suite runs against the artefact users install.

   WHY A COMPILE FLAG AND NOT JUST THE ENVIRONMENT VARIABLE. With
   --enable-fpmng-debug-clock off -- the default, and what every shipped package
   is built with -- none of this is in the binary, so there is no variable to set
   and no way to skew a production master's sense of time. That is the objection
   that should kill a test-only knob, and gating it at compile time answers it.

   WHAT IS SCALED, AND WHAT IS DELIBERATELY NOT. Both CLOCK_REALTIME and
   CLOCK_MONOTONIC, by the same rate. Faking only one would invent a skew that
   the shipped binary cannot reach: fpm_pool_supervisor.c measures the
   fast-restart streak on monotonic milliseconds and supervisor.restart_delay /
   next_allowed_start on time(NULL), so a real monotonic clock next to a
   10x realtime one would make restarts look "fast" that are not, and
   fpmng-supervisor-fast-restart would be testing a state that cannot occur.

   NOT scaled: the two clock_gettime() calls that exist only as hash entropy
   (fpm_pool_cron.c and fpm_pool_supervisor.c, both mixed with getpid()). They
   are not measuring anything, and a scaled reading there would only narrow the
   input range.

   The anchor is taken once in fpm_init(), before anything forks, so the master
   and every child it forks agree on the same virtual clock.

   AT RATE 1 -- unset, or explicitly 1 -- every function here is the plain
   libc call and nothing is logged, so a debug-clock build with the variable
   unset behaves identically to one without the flag. That is what keeps the
   other 120 tests in the suite unaffected. */

#ifndef FPM_DEBUG_CLOCK_H
#define FPM_DEBUG_CLOCK_H 1

#include <time.h>
#include <unistd.h>

#ifdef HAVE_FPMNG_DEBUG_CLOCK

/* An upper bound, not a tuning knob: past this the virtual clock outruns the
 * one-second resolution the schedule is expressed in, and a "* * * * *" pool
 * would start skipping minutes instead of running faster. */
#define FPM_DEBUG_CLOCK_MAX_RATE 600

void fpm_debug_clock_init(void);
time_t fpm_debug_clock_now(void);
int fpm_debug_clock_monotonic(struct timespec *ts);
void fpm_debug_clock_sleep(time_t seconds);

#define FPM_NOW()             fpm_debug_clock_now()
#define FPM_MONOTONIC(tsp)    fpm_debug_clock_monotonic(tsp)
#define FPM_SLEEP(seconds)    fpm_debug_clock_sleep(seconds)

#else

/* Macros rather than wrapper functions so that a default build carries no new
 * symbol, no call and no branch: the preprocessor leaves the original libc
 * call behind, byte for byte. */
#define FPM_NOW()             time(NULL)
#define FPM_MONOTONIC(tsp)    clock_gettime(CLOCK_MONOTONIC, (tsp))
#define FPM_SLEEP(seconds)    ((void) sleep((unsigned) (seconds)))

#define fpm_debug_clock_init() ((void) 0)

#endif

#endif
