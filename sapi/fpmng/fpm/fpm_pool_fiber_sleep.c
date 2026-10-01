/* fpm-ng: pool.executor = fiber — sleep()/usleep()/time_nanosleep() without
 * blocking the process. See fpm_pool_fiber_sleep.h.
 *
 * Mechanism: one TIMER operation through the IO seam (fpm_pool_fiber_io.h).
 * Its backend (fpm_pool_fiber.c:fpm_pool_fiber_wait_wake) suspends the request
 * Fiber on the scheduler's own per-request event with a timeout, and the
 * scheduler cleans that up (event_del after return, event_free when the Fiber
 * dies — exactly as for every POLL from the transport layer). Therefore do NOT
 * create a separate timer here: a second clock would duplicate what the
 * backend already does with one timer, and that SECOND path would itself need
 * a cleanup hook (similar to fpm_coop_req_free) for a Fiber destroyed with a
 * timer pending (see docs/spike-sleep-yield-report.md, "Cleanup safety").
 *
 * The waker pair (fpm_fiber_io_waker()/fpm_fiber_io_wake()) is not needed:
 * sleep has no external interruption source in this model, so the TIMER's
 * CANCELLED completion is handled (an upstream provider may produce it) but the
 * libevent backend never returns it.
 */

#include "fpm_config.h"

#include <string.h>
#include <sys/time.h>

#include "php.h"
#include "zend_API.h"
#include "zend_exceptions.h"

#include "fpm_pool_fiber_io.h"
#include "fpm_pool_fiber_sleep.h"
#include "zlog.h"

static zif_handler fpm_fiber_sleep_orig_sleep;
static zif_handler fpm_fiber_sleep_orig_usleep;
static zif_handler fpm_fiber_sleep_orig_time_nanosleep;

static void fpm_pool_fiber_sleep_install(void);

struct fpm_fiber_intercept_s fpm_fiber_sleep_intercept = {
	.name = "sleep",
	.install = fpm_pool_fiber_sleep_install,
};

/* Suspend the request for *tv. Returns the completion; UNSUPPORTED = nothing
 * was suspended, the caller runs the original blocking handler. */
static enum fpm_fiber_io_completion fpm_fiber_sleep_timer(struct timeval *tv) /* {{{ */
{
	struct fpm_fiber_io_op_s op;

	memset(&op, 0, sizeof(op));
	op.type = FPM_FIBER_IO_OP_TIMER;
	op.timeout = tv;
	return fpm_fiber_io_run(&fpm_fiber_sleep_intercept, &op);
}
/* }}} */

/* Replace the handler of one internal function; *orig receives the original.
 * 0 = replaced, -1 = the function is missing (another platform/build) or is
 * not internal — the caller logs it and simply does not hook that function. */
static int fpm_fiber_sleep_swap(const char *name, size_t name_len, zif_handler repl, zif_handler *orig) /* {{{ */
{
	zend_function *fn = zend_hash_str_find_ptr(CG(function_table), name, name_len);

	if (!fn || fn->type != ZEND_INTERNAL_FUNCTION) {
		return -1;
	}
	*orig = fn->internal_function.handler;
	fn->internal_function.handler = repl;
	return 0;
}
/* }}} */

/* --- sleep() ---------------------------------------------------------------- */

static ZEND_FASTCALL void fpm_fiber_zif_sleep(INTERNAL_FUNCTION_PARAMETERS) /* {{{ */
{
	zend_long num;
	const unsigned int max = UINT_MAX;	/* target platform: Linux, not Windows */
	struct timeval tv, start, deadline;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(num)
	ZEND_PARSE_PARAMETERS_END();

	if (num < 0 || (zend_ulong) num > max) {
		zend_argument_value_error(1, "must be between 0 and %u", max);
		RETURN_THROWS();
	}

	if (!fpm_fiber_io_can_suspend(&fpm_fiber_sleep_intercept)) {
		fpm_fiber_sleep_orig_sleep(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}

	tv.tv_sec = (time_t) num;
	tv.tv_usec = 0;
	gettimeofday(&start, NULL);
	timeradd(&start, &tv, &deadline);

	switch (fpm_fiber_sleep_timer(&tv)) {
		case FPM_FIBER_IO_UNSUPPORTED:
			/* can_suspend() changed its mind between the check and the call
			 * (should not happen in single-threaded code) — do not lose the
			 * sleep time; call the real blocking sleep(). */
			fpm_fiber_sleep_orig_sleep(INTERNAL_FUNCTION_PARAM_PASSTHRU);
			return;
		case FPM_FIBER_IO_CANCELLED: {
			/* Woken early: upstream returns the seconds left on EINTR. */
			struct timeval now;

			gettimeofday(&now, NULL);
			if (timercmp(&deadline, &now, >)) {
				struct timeval rem;

				timersub(&deadline, &now, &rem);
				RETURN_LONG((zend_long) rem.tv_sec + (rem.tv_usec ? 1 : 0));
			}
			RETURN_LONG(0);
		}
		default:
			/* The entire requested time elapsed — an uninterrupted sleep(). */
			RETURN_LONG(0);
	}
}
/* }}} */

/* --- usleep() ----------------------------------------------------------------- */

static ZEND_FASTCALL void fpm_fiber_zif_usleep(INTERNAL_FUNCTION_PARAMETERS) /* {{{ */
{
	zend_long num;
	struct timeval tv;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_LONG(num)
	ZEND_PARSE_PARAMETERS_END();

	if (num < 0 || (zend_ulong) num > UINT_MAX) {
		zend_argument_value_error(1, "must be between 0 and %u", UINT_MAX);
		RETURN_THROWS();
	}

	if (!fpm_fiber_io_can_suspend(&fpm_fiber_sleep_intercept)) {
		fpm_fiber_sleep_orig_usleep(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}

	tv.tv_sec = (time_t) (num / 1000000);
	tv.tv_usec = (suseconds_t) (num % 1000000);

	if (fpm_fiber_sleep_timer(&tv) == FPM_FIBER_IO_UNSUPPORTED) {
		fpm_fiber_sleep_orig_usleep(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	}
	/* usleep() returns nothing (void) on every branch, CANCELLED included. */
}
/* }}} */

/* --- time_nanosleep() ---------------------------------------------------------- */

#ifdef HAVE_NANOSLEEP

static ZEND_FASTCALL void fpm_fiber_zif_time_nanosleep(INTERNAL_FUNCTION_PARAMETERS) /* {{{ */
{
	zend_long tv_sec, tv_nsec;
	struct timeval tv;
	enum fpm_fiber_io_completion done;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_LONG(tv_sec)
		Z_PARAM_LONG(tv_nsec)
	ZEND_PARSE_PARAMETERS_END();

	if (tv_sec < 0) {
		zend_argument_value_error(1, "must be greater than or equal to 0");
		RETURN_THROWS();
	}
	if (tv_nsec < 0) {
		zend_argument_value_error(2, "must be greater than or equal to 0");
		RETURN_THROWS();
	}

	if (!fpm_fiber_io_can_suspend(&fpm_fiber_sleep_intercept)) {
		fpm_fiber_sleep_orig_time_nanosleep(INTERNAL_FUNCTION_PARAM_PASSTHRU);
		return;
	}

	/* Upstream does not check the upper bound of tv_nsec itself — nanosleep()
	 * discovers it (EINVAL) and then throws EXACTLY this message. We do not call
	 * nanosleep(), so check the same condition ourselves to preserve the same
	 * error contract. */
	if (tv_nsec > 999999999L) {
		zend_value_error("Nanoseconds was not in the range 0 to 999 999 999 or seconds was negative");
		RETURN_THROWS();
	}

	tv.tv_sec = (time_t) tv_sec;
	tv.tv_usec = (suseconds_t) (tv_nsec / 1000);

	{
		struct timeval start, deadline;

		gettimeofday(&start, NULL);
		timeradd(&start, &tv, &deadline);

		done = fpm_fiber_sleep_timer(&tv);
		if (done == FPM_FIBER_IO_UNSUPPORTED) {
			fpm_fiber_sleep_orig_time_nanosleep(INTERNAL_FUNCTION_PARAM_PASSTHRU);
			return;
		}

		if (done != FPM_FIBER_IO_CANCELLED) {
			RETURN_TRUE;
		}

		/* CANCELLED: woken early — the upstream EINTR contract is an array of
		 * seconds/nanoseconds remaining. We do not have nanosleep()'s real "rem",
		 * so calculate it as deadline minus now — dead with the libevent backend
		 * (see the file header), but correct for a provider that cancels. */
		{
			struct timeval now, rem;

			gettimeofday(&now, NULL);
			if (timercmp(&deadline, &now, >)) {
				timersub(&deadline, &now, &rem);
			} else {
				rem.tv_sec = 0;
				rem.tv_usec = 0;
			}
			array_init(return_value);
			add_assoc_long_ex(return_value, "seconds", sizeof("seconds") - 1, rem.tv_sec);
			add_assoc_long_ex(return_value, "nanoseconds", sizeof("nanoseconds") - 1, rem.tv_usec * 1000);
		}
	}
}
/* }}} */

#endif /* HAVE_NANOSLEEP */

static void fpm_pool_fiber_sleep_install(void) /* {{{ */
{
	if (fpm_fiber_sleep_swap("sleep", sizeof("sleep") - 1, fpm_fiber_zif_sleep, &fpm_fiber_sleep_orig_sleep) < 0) {
		zlog(ZLOG_WARNING, "fiber: sleep() not found as an internal function, not intercepting it");
	}
	if (fpm_fiber_sleep_swap("usleep", sizeof("usleep") - 1, fpm_fiber_zif_usleep, &fpm_fiber_sleep_orig_usleep) < 0) {
		zlog(ZLOG_WARNING, "fiber: usleep() not found as an internal function, not intercepting it");
	}
#ifdef HAVE_NANOSLEEP
	if (fpm_fiber_sleep_swap("time_nanosleep", sizeof("time_nanosleep") - 1, fpm_fiber_zif_time_nanosleep, &fpm_fiber_sleep_orig_time_nanosleep) < 0) {
		zlog(ZLOG_WARNING, "fiber: time_nanosleep() not found as an internal function, not intercepting it");
	}
#endif
}
/* }}} */
