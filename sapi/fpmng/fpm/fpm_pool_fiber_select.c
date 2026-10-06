/* fpm-ng: pool.executor = fiber — make stream_select() suspend the request
 * Fiber instead of blocking the process. See fpm_pool_fiber_select.h and
 * task 006 (done; see docs/task-archive.md).
 *
 * stream_select() takes SETS of descriptors from userland
 * (ext/standard/streamsfuncs.c: PHP_FUNCTION(stream_select)), unlike the
 * per-stream interception in fpm_pool_fiber_xport.c. The patch
 * (async/patches/0008-fiber-stream-select.patch, HAVE_FPMNG_FIBER) replaces exactly one
 * call, php_select(max_fd+1, &rfds, &wfds, &efds, tv_p), with
 * fpm_fiber_select() below — same signature, same postconditions. Everything
 * else in that function (argument parsing, building rfds/wfds/efds from the
 * stream arrays, narrowing them back afterwards) is untouched upstream code.
 *
 * Mechanism: one ANY operation through the IO seam (fpm_pool_fiber_io.h), one
 * member per candidate fd. The seam's backend registers one event per member
 * and resumes the Fiber when the first one fires or the timeout passes
 * (fpm_pool_fiber_io.c, "ANY"); this file only translates fd_sets to members
 * and back.
 *
 * Not handled here, on purpose:
 * - efds (the "exceptional conditions" set): select()'s meaning for it is
 *   mostly OOB TCP data, which libevent has no portable equivalent for.
 *   Guessing at it would violate the "never change what the function reports"
 *   acceptance criterion, so a non-empty efds falls back to the real,
 *   process-blocking select() — same as calling stream_select() would have
 *   done before this file existed. Predis and php-amqplib do not use it.
 * - fd_select on the async executor: tracked separately (see task 006).
 */

#include "fpm_config.h"

#include <errno.h>
#include <string.h>
#include <sys/select.h>

#include "php.h"

#include "fpm_pool_coop.h"
#include "fpm_pool_fiber_io.h"
#include "fpm_pool_fiber_select.h"
#include "zlog.h"

/* Nothing to install: patch 0008 compiles the call to fpm_fiber_select() into
 * stream_select(). The registry entry exists so fiber.disable_interceptions =
 * select reaches that call site through fpm_fiber_io_can_suspend(). */
struct fpm_fiber_intercept_s fpm_fiber_select_intercept = {
	.name = "select",
};

/* Once per process: efds is rare enough (neither Predis nor php-amqplib use
 * it) that falling back silently every time would be surprising in the log,
 * but logging every call would spam it under a library that does pass it. */
static bool fpm_fiber_select_efds_warned = false;

int fpm_fiber_select(int max_fd, fd_set *rfds, fd_set *wfds, fd_set *efds, struct timeval *timeout) /* {{{ */
{
	struct fpm_fiber_io_poll_s *members;
	struct fpm_fiber_io_op_s op;
	int fd, i, n = 0, ready;
	bool poll_only = timeout && timeout->tv_sec == 0 && timeout->tv_usec == 0;
	bool have_efds = false;

	if (efds) {
		for (fd = 0; fd < max_fd; fd++) {
			if (FD_ISSET(fd, efds)) {
				have_efds = true;
				break;
			}
		}
	}

	if (have_efds || poll_only || !fpm_fiber_io_can_suspend(&fpm_fiber_select_intercept)) {
		if (have_efds && !fpm_fiber_select_efds_warned) {
			fpm_fiber_select_efds_warned = true;
			zlog(ZLOG_DEBUG, "[pool %s] fiber: stream_select() with a non-empty "
							 "exceptfds set falls back to blocking select() (once per process)",
					fpm_coop_pool_name());
		}
		return select(max_fd, rfds, wfds, efds, timeout);
	}

	members = ecalloc(max_fd > 0 ? max_fd : 1, sizeof(*members));

	for (fd = 0; fd < max_fd; fd++) {
		unsigned want = 0;

		if (rfds && FD_ISSET(fd, rfds)) {
			want |= FPM_FIBER_IO_READ;
		}
		if (wfds && FD_ISSET(fd, wfds)) {
			want |= FPM_FIBER_IO_WRITE;
		}
		if (!want) {
			continue;
		}
		members[n].fd = fd;
		members[n].events = want;
		n++;
	}

	if (n == 0) {
		/* stream_array_to_fd_set already refused an empty set of arrays
		 * before this is reached; this is defensive, not a real path. */
		efree(members);
		return select(max_fd, rfds, wfds, efds, timeout);
	}

	memset(&op, 0, sizeof(op));
	op.type = FPM_FIBER_IO_OP_ANY;
	op.timeout = timeout;
	op.u.any.members = members;
	op.u.any.count = n;

	if (fpm_fiber_io_run(&fpm_fiber_select_intercept, &op) == FPM_FIBER_IO_UNSUPPORTED) {
		/* Nothing was waited for (can_suspend() changed its mind between the
		 * check above and here, which should not happen in single-threaded
		 * code) — do not lose the wait, fall back to the real, blocking
		 * select(). */
		efree(members);
		return select(max_fd, rfds, wfds, efds, timeout);
	}

	/* READY or TIMEOUT: the sets become the ready subset (empty on TIMEOUT).
	 * CANCELLED is never produced for ANY by the libevent backend; treating it
	 * like TIMEOUT (nothing ready, return 0) is select()'s own answer to a
	 * wait that ended without an event. */
	if (rfds) {
		FD_ZERO(rfds);
	}
	if (wfds) {
		FD_ZERO(wfds);
	}
	if (efds) {
		FD_ZERO(efds);
	}

	ready = 0;
	for (i = 0; i < n; i++) {
		if ((members[i].revents & FPM_FIBER_IO_READ) && rfds) {
			FD_SET(members[i].fd, rfds);
			ready++;
		}
		if ((members[i].revents & FPM_FIBER_IO_WRITE) && wfds) {
			FD_SET(members[i].fd, wfds);
			ready++;
		}
	}

	efree(members);
	return ready;
}
/* }}} */
