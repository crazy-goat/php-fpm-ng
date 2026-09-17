/* fpm-ng: a virtual clock for the test suite (issue #396). The whole rationale
 * -- and in particular why this is a compile flag, why both clocks are scaled
 * and which two readings are deliberately left alone -- is in
 * fpm_debug_clock.h. Read that first. */

#include "fpm_config.h"
#include "fpm_debug_clock.h"

#ifdef HAVE_FPMNG_DEBUG_CLOCK

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include "zlog.h"

#define FPM_DEBUG_CLOCK_ENV "FPMNG_DEBUG_CLOCK_RATE"
#define FPM_DEBUG_CLOCK_NS_PER_S 1000000000LL

/* 1 = real speed. Every entry point below short-circuits to the plain libc call
 * at 1, so an unset variable costs one comparison and changes nothing. */
static unsigned fpm_debug_clock_rate = 1;
static struct timespec fpm_debug_clock_anchor_real;
static struct timespec fpm_debug_clock_anchor_mono;

static int64_t fpm_debug_clock_ns(const struct timespec *ts) /* {{{ */
{
	return (int64_t) ts->tv_sec * FPM_DEBUG_CLOCK_NS_PER_S + (int64_t) ts->tv_nsec;
}
/* }}} */

/* Nanoseconds of virtual time since `anchor`, given the current reading `now`.
 * Clamped at zero: CLOCK_REALTIME can be stepped backwards by the operator (or
 * by NTP) and a negative elapsed time would make the virtual clock run
 * backwards, which is worse than briefly standing still. */
static int64_t fpm_debug_clock_elapsed(const struct timespec *anchor,
		const struct timespec *now) /* {{{ */
{
	int64_t elapsed = fpm_debug_clock_ns(now) - fpm_debug_clock_ns(anchor);

	if (elapsed < 0) {
		elapsed = 0;
	}

	return elapsed * (int64_t) fpm_debug_clock_rate;
}
/* }}} */

void fpm_debug_clock_init(void) /* {{{ */
{
	const char *raw = getenv(FPM_DEBUG_CLOCK_ENV);
	unsigned long parsed;
	char *end;

	fpm_debug_clock_rate = 1;

	if (!raw || !*raw) {
		return;
	}

	/* Both anchors, or neither: a rate applied to only one of the two clocks
	 * is exactly the skew this file exists to avoid. */
	if (clock_gettime(CLOCK_REALTIME, &fpm_debug_clock_anchor_real) != 0 ||
	    clock_gettime(CLOCK_MONOTONIC, &fpm_debug_clock_anchor_mono) != 0) {
		zlog(ZLOG_WARNING, FPM_DEBUG_CLOCK_ENV " is set but clock_gettime() failed; "
			"the clock runs at real speed");
		return;
	}

	errno = 0;
	parsed = strtoul(raw, &end, 10);
	if (errno != 0 || end == raw || *end != '\0' ||
	    parsed < 1 || parsed > FPM_DEBUG_CLOCK_MAX_RATE) {
		zlog(ZLOG_WARNING, FPM_DEBUG_CLOCK_ENV " = '%s' is not an integer in 1..%d; "
			"the clock runs at real speed", raw, FPM_DEBUG_CLOCK_MAX_RATE);
		return;
	}

	fpm_debug_clock_rate = (unsigned) parsed;
}
/* }}} */

time_t fpm_debug_clock_now(void) /* {{{ */
{
	struct timespec now;

	if (fpm_debug_clock_rate <= 1) {
		return time(NULL);
	}

	if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
		return time(NULL);
	}

	return (time_t) (fpm_debug_clock_anchor_real.tv_sec
		+ fpm_debug_clock_elapsed(&fpm_debug_clock_anchor_real, &now) / FPM_DEBUG_CLOCK_NS_PER_S);
}
/* }}} */

int fpm_debug_clock_monotonic(struct timespec *ts) /* {{{ */
{
	struct timespec now;
	int64_t total;

	if (fpm_debug_clock_rate <= 1) {
		return clock_gettime(CLOCK_MONOTONIC, ts);
	}

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return -1;
	}

	total = fpm_debug_clock_ns(&fpm_debug_clock_anchor_mono)
		+ fpm_debug_clock_elapsed(&fpm_debug_clock_anchor_mono, &now);

	ts->tv_sec = (time_t) (total / FPM_DEBUG_CLOCK_NS_PER_S);
	ts->tv_nsec = (long) (total % FPM_DEBUG_CLOCK_NS_PER_S);

	return 0;
}
/* }}} */

/* `seconds` is VIRTUAL seconds, the unit every caller already thinks in: the
 * schedule says "sleep until the next minute", and at rate 10 that is six real
 * seconds. nanosleep() rather than sleep() because the real interval is
 * fractional, and -- like sleep() -- it is interrupted by a delivered signal,
 * which fpm_pool_cron_sleep_until() and fpm_pool_supervisor_wait() both rely on
 * to notice SIGTERM. Both re-check their own flag and call again, so returning
 * early on EINTR is correct here and needs no loop of its own. */
void fpm_debug_clock_sleep(time_t seconds) /* {{{ */
{
	struct timespec req;
	int64_t real_ns;

	if (seconds <= 0) {
		return;
	}

	if (fpm_debug_clock_rate <= 1) {
		(void) sleep((unsigned) seconds);
		return;
	}

	real_ns = ((int64_t) seconds * FPM_DEBUG_CLOCK_NS_PER_S) / (int64_t) fpm_debug_clock_rate;

	req.tv_sec = (time_t) (real_ns / FPM_DEBUG_CLOCK_NS_PER_S);
	req.tv_nsec = (long) (real_ns % FPM_DEBUG_CLOCK_NS_PER_S);

	(void) nanosleep(&req, NULL);
}
/* }}} */

#endif
