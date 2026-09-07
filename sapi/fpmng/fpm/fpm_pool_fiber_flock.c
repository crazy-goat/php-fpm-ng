/* fpm-ng: pool.executor = fiber — flock() spike. See fpm_pool_fiber_flock.h. */

#include "fpm_config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>	/* LOCK_SH/LOCK_EX/LOCK_UN/LOCK_NB — standard BSD constants, as in ext/standard/flock_compat.h */
#include <sys/stat.h>
#include <sys/types.h>

#include "php.h"
#include "main/php_streams.h"
#include "main/streams/php_stream_plain_wrapper.h"

#include "fpm_pool_fiber.h"
#include "fpm_pool_fiber_flock.h"
#include "zlog.h"

/* One process = one OS thread; the scheduler switches Fibers ONLY at explicit
 * suspension points (fpm_pool_fiber_wait_wake/wait_fd). Nothing else touches
 * this registry between two such points — no locks are needed. If this file
 * ever also runs under an executor other than fiber (async with OS threads?),
 * THIS assumption must be reconsidered from scratch. */

#define FPM_FLOCK_MAX_ENTRIES   4096
#define FPM_FLOCK_MAX_SH        64
#define FPM_FLOCK_MAX_WAITERS   64

/* Default NB+retry polling parameters for INTER-PROCESS contention (another
 * process holds the kernel lock — there is no readiness event, so polling is
 * inherent). Overridable through an environment variable for measurements
 * without a rebuild: FPMNG_FLOCK_POLL_ATTEMPTS=0 disables polling completely
 * (the first NB failure falls straight through to the real blocking flock()) —
 * to compare the cost of "poll then block" with "just block". */
#define FPM_FLOCK_POLL_ATTEMPTS_DEFAULT 5
#define FPM_FLOCK_POLL_INTERVAL_USEC    20000	/* 20 ms */

struct fpm_flock_entry_s {
	bool used;
	dev_t dev;
	ino_t ino;
	void *ex_owner;			/* Fiber waiter handle holding LOCK_EX in THIS process, or NULL */
	void *sh_owners[FPM_FLOCK_MAX_SH];
	int sh_count;
	void *waiters[FPM_FLOCK_MAX_WAITERS];	/* Fibers waiting for SOMETHING on this file in this process */
	int n_waiters;
};

static struct fpm_flock_entry_s fpm_flock_entries[FPM_FLOCK_MAX_ENTRIES];
static int fpm_flock_entry_count = 0;
static bool fpm_flock_table_full_warned = false;
static bool fpm_flock_waiters_full_warned = false;

static int (*fpm_flock_orig_set_option)(php_stream *stream, int option, int value, void *ptrparam);
static bool fpm_flock_installed = false;

static int fpm_flock_poll_attempts = -1;	/* -1 = not read from env yet */

static int fpm_flock_poll_attempts_get(void) /* {{{ */
{
	if (fpm_flock_poll_attempts < 0) {
		const char *env = getenv("FPMNG_FLOCK_POLL_ATTEMPTS");

		fpm_flock_poll_attempts = env ? atoi(env) : FPM_FLOCK_POLL_ATTEMPTS_DEFAULT;
		if (fpm_flock_poll_attempts < 0) {
			fpm_flock_poll_attempts = 0;
		}
	}
	return fpm_flock_poll_attempts;
}
/* }}} */

/* --- registry --------------------------------------------------------- */

static struct fpm_flock_entry_s *fpm_flock_find(dev_t dev, ino_t ino, bool create) /* {{{ */
{
	int i;
	struct fpm_flock_entry_s *e;

	for (i = 0; i < fpm_flock_entry_count; i++) {
		e = &fpm_flock_entries[i];
		if (e->used && e->dev == dev && e->ino == ino) {
			return e;
		}
	}
	if (!create) {
		return NULL;
	}
	if (fpm_flock_entry_count == FPM_FLOCK_MAX_ENTRIES) {
		if (!fpm_flock_table_full_warned) {
			fpm_flock_table_full_warned = true;
			zlog(ZLOG_WARNING, "fiber: flock registry full (%d files locked at once in one process); "
				"falling back to the plain flock() syscall for further files — no in-process "
				"suspend-instead-of-block for them, but correctness is unaffected", FPM_FLOCK_MAX_ENTRIES);
		}
		return NULL;
	}
	e = &fpm_flock_entries[fpm_flock_entry_count++];
	memset(e, 0, sizeof(*e));
	e->used = true;
	e->dev = dev;
	e->ino = ino;
	return e;
}
/* }}} */

static bool fpm_flock_conflict(const struct fpm_flock_entry_s *e, void *owner, int mode) /* {{{ */
{
	int i;

	if (e->ex_owner && e->ex_owner != owner) {
		return true;
	}
	if (mode == LOCK_EX) {
		for (i = 0; i < e->sh_count; i++) {
			if (e->sh_owners[i] != owner) {
				return true;
			}
		}
	}
	/* LOCK_SH: shares with other LOCK_SH holders; conflicts only with ex_owner (already checked). */
	return false;
}
/* }}} */

static void fpm_flock_sh_remove(struct fpm_flock_entry_s *e, void *owner) /* {{{ */
{
	int i;

	for (i = 0; i < e->sh_count; i++) {
		if (e->sh_owners[i] == owner) {
			e->sh_owners[i] = e->sh_owners[e->sh_count - 1];
			e->sh_count--;
			return;
		}
	}
}
/* }}} */

static void fpm_flock_sh_add(struct fpm_flock_entry_s *e, void *owner) /* {{{ */
{
	int i;

	for (i = 0; i < e->sh_count; i++) {
		if (e->sh_owners[i] == owner) {
			return;	/* already on the list */
		}
	}
	if (e->sh_count == FPM_FLOCK_MAX_SH) {
		/* Record number of shared readers of the same file in one process. The
		 * registry does not track this one additional reader — its future LOCK_UN
		 * simply will not find itself on the list (no-op), and a competitor for its
		 * LOCK_EX may NOT be suspended in this process (it will fail at the real
		 * flock(), as today). This does not compromise kernel-level safety; it only
		 * loses this one optimization. */
		return;
	}
	e->sh_owners[e->sh_count++] = owner;
}
/* }}} */

static void fpm_flock_register_holder(struct fpm_flock_entry_s *e, void *owner, int mode) /* {{{ */
{
	if (mode == LOCK_EX) {
		fpm_flock_sh_remove(e, owner);	/* upgrade SH->EX for the same owner, if it held SH */
		e->ex_owner = owner;
	} else { /* LOCK_SH */
		if (e->ex_owner == owner) {
			e->ex_owner = NULL;	/* downgrade EX->SH for the same owner */
		}
		fpm_flock_sh_add(e, owner);
	}
}
/* }}} */

static void fpm_flock_wake_all(struct fpm_flock_entry_s *e) /* {{{ */
{
	int i;

	for (i = 0; i < e->n_waiters; i++) {
		fpm_pool_fiber_wake(e->waiters[i]);
	}
	e->n_waiters = 0;
}
/* }}} */

static bool fpm_flock_add_waiter(struct fpm_flock_entry_s *e, void *owner) /* {{{ */
{
	if (e->n_waiters == FPM_FLOCK_MAX_WAITERS) {
		if (!fpm_flock_waiters_full_warned) {
			fpm_flock_waiters_full_warned = true;
			zlog(ZLOG_WARNING, "fiber: flock wait queue full for one file (%d waiters); "
				"failing the lock attempt for the overflow waiter with EWOULDBLOCK instead of "
				"blocking the process on a same-process holder", FPM_FLOCK_MAX_WAITERS);
		}
		return false;
	}
	e->waiters[e->n_waiters++] = owner;
	return true;
}
/* }}} */

void fpm_pool_fiber_flock_release_owner(void *owner) /* {{{ */
{
	int i;

	if (!owner) {
		return;
	}
	for (i = 0; i < fpm_flock_entry_count; i++) {
		struct fpm_flock_entry_s *e = &fpm_flock_entries[i];
		bool held = false;

		if (!e->used) {
			continue;
		}
		if (e->ex_owner == owner) {
			e->ex_owner = NULL;
			held = true;
		}
		if (e->sh_count > 0) {
			int before = e->sh_count;

			fpm_flock_sh_remove(e, owner);
			held = held || (e->sh_count != before);
		}
		if (held) {
			/* This owner held this file and has just disappeared (request end,
			 * including the "it broke" case — see the file header). The real
			 * kernel lock is released when the engine closes its fd (the stream
			 * destructor), independently of this entry. This entry is ONLY our
			 * bookkeeping; if we do not clear it, every future competitor in this
			 * process will suspend here forever (exactly the bug this code removes). */
			fpm_flock_wake_all(e);
		}
		/* n_waiters: this owner may also have been IN THE QUEUE (rather than
		 * holding a lock) when its request ended (for example, a request removed
		 * while waiting — this should not happen today because wait_wake has no
		 * timeout while waiting for release, but clean it up defensively so a dead
		 * pointer is never woken or compared). */
		{
			int j;
			for (j = 0; j < e->n_waiters; j++) {
				if (e->waiters[j] == owner) {
					e->waiters[j] = e->waiters[e->n_waiters - 1];
					e->n_waiters--;
					j--;
				}
			}
		}
	}
}
/* }}} */

/* --- hook --------------------------------------------------------------- */

static int fpm_flock_real(php_stream *stream, int value, void *ptrparam) /* {{{ */
{
	return fpm_flock_orig_set_option(stream, PHP_STREAM_OPTION_LOCKING, value, ptrparam);
}
/* }}} */

static int fpm_flock_fd_identity(php_stream *stream, dev_t *dev, ino_t *ino) /* {{{ */
{
	zend_result cast_ret;
	int fd = -1;
	struct stat st;

	cast_ret = php_stream_cast(stream, PHP_STREAM_AS_FD, (void **) &fd, 0);
	if (cast_ret != SUCCESS || fd < 0) {
		return -1;
	}
	if (fstat(fd, &st) != 0) {
		return -1;
	}
	*dev = st.st_dev;
	*ino = st.st_ino;
	return 0;
}
/* }}} */

static int fpm_fiber_flock_set_option(php_stream *stream, int option, int value, void *ptrparam) /* {{{ */
{
	dev_t dev;
	ino_t ino;
	int mode, nb, ret, attempts, max_attempts;
	void *owner;
	struct fpm_flock_entry_s *entry;

	if (option != PHP_STREAM_OPTION_LOCKING) {
		return fpm_flock_orig_set_option(stream, option, value, ptrparam);
	}
	if ((uintptr_t) ptrparam == PHP_STREAM_LOCK_SUPPORTED) {
		/* Support query (php_stream_supports_lock), not a locking operation. */
		return fpm_flock_orig_set_option(stream, option, value, ptrparam);
	}

	owner = fpm_pool_fiber_waiter();
	if (!owner) {
		/* Outside a Fiber request context (for example, the container script
		 * before the first request) — a process registry is not meaningful; retain
		 * the exact original behavior. */
		return fpm_flock_orig_set_option(stream, option, value, ptrparam);
	}

	mode = value & ~LOCK_NB;
	nb = value & LOCK_NB;

	if (mode == LOCK_UN) {
		ret = fpm_flock_real(stream, value, ptrparam);
		if (fpm_flock_fd_identity(stream, &dev, &ino) == 0) {
			entry = fpm_flock_find(dev, ino, false);
			if (entry) {
				bool was_ex = (entry->ex_owner == owner);

				if (was_ex) {
					entry->ex_owner = NULL;
				}
				fpm_flock_sh_remove(entry, owner);
				fpm_flock_wake_all(entry);
			}
		}
		return ret;
	}

	if (fpm_flock_fd_identity(stream, &dev, &ino) != 0) {
		/* We cannot identify the file (fstat failed) — there is no safe way to
		 * maintain a registry keyed by dev+inode; retain the exact original
		 * behavior (still correct, only without suspending instead of blocking). */
		return fpm_flock_orig_set_option(stream, option, value, ptrparam);
	}

	entry = fpm_flock_find(dev, ino, true);
	if (!entry) {
		return fpm_flock_orig_set_option(stream, option, value, ptrparam);
	}

	/* NOTE (found and fixed DURING this spike, see the report): the in-process
	 * phase and the inter-process phase MUST be one loop that rechecks the
	 * in-process conflict from scratch BEFORE every next real flock() attempt —
	 * not two separate phases run once. The first version of this file used two
	 * phases: (1) check the in-process conflict ONCE, (2) if there was no
	 * conflict, poll the real flock() until attempts were exhausted, then block
	 * in the real flock(). Between retries in phase (2), the Fiber YIELDS the
	 * processor (fpm_pool_fiber_wait_wake). During that window ANOTHER Fiber IN
	 * THE SAME process could enter, also see no conflict (because the first
	 * Fiber held nothing yet), win the real flock(), and suspend on a socket
	 * (correctly). When the first Fiber returned from polling, it still saw
	 * EWOULDBLOCK (because someone NOW held the lock in the same process), but its
	 * polling loop no longer checked the in-process registry — after exhausting
	 * attempts it entered the REAL BLOCKING flock(), freezing the entire OS
	 * thread. Because the holder was a Fiber IN THE SAME process suspended on a
	 * socket, it could never regain the processor to finish and release — a
	 * permanent deadlock, measured (two separate processes both paralyzed; gdb on
	 * the live process showed the frame exactly in the real blocking flock(), with
	 * entry->ex_owner set to ANOTHER Fiber IN THE SAME process).
	 * Fix: every iteration first checks the in-process registry (and enters a
	 * real suspension if there is a conflict), and ONLY when nobody in this
	 * process holds a conflicting lock does it try the real flock(). */
	max_attempts = fpm_flock_poll_attempts_get();
	attempts = 0;
	for (;;) {
		/* In-process: while SOMEONE ELSE in THIS process holds a conflicting
		 * mode, suspend on the in-memory queue — no polling, real wait/wake,
		 * awakened by the holder's LOCK_UN (fpm_flock_wake_all). */
		while (fpm_flock_conflict(entry, owner, mode)) {
			if (nb) {
				errno = EWOULDBLOCK;
				return -1;	/* caller requested LOCK_NB — do not suspend or block */
			}
			if (!fpm_pool_fiber_can_wait() || !fpm_flock_add_waiter(entry, owner)) {
				/* FIX (was: fall through to a real BLOCKING flock() here).
				 * We just confirmed, above, that another fiber IN THIS SAME
				 * PROCESS holds a conflicting lock. That holder cannot run
				 * again — the one OS thread this process has is about to
				 * be the caller of a blocking flock() — until this call
				 * returns, so a real blocking flock() here can never be
				 * satisfied: permanent, whole-process deadlock, exactly the
				 * bug this file exists to remove, just reached from the
				 * "can't suspend" or "queue full" edge instead of the
				 * two-phase-loop edge fixed earlier in this same spike (see
				 * the report). There is no safe blocking fallback for an
				 * in-process conflict, ever, by construction: the process
				 * has one thread and the holder is one of this process's
				 * own fibers.
				 *
				 * So: fail the lock attempt instead, exactly the way a
				 * real non-blocking flock() fails when it cannot be
				 * granted right away (see the `nb` branch a few lines
				 * above, and php_flock_common() in ext/standard/file.c:
				 * flock($fp, LOCK_EX) returns false and, if $wouldblock
				 * was passed, sets it to true when errno is EWOULDBLOCK;
				 * file_put_contents(..., LOCK_EX) returns false with an
				 * E_WARNING). This is documented, ordinary, userland-
				 * visible lock failure, not a crash and not silent data
				 * loss — it is materially better than losing the whole
				 * worker and every request in flight.
				 *
				 * This applies identically to BOTH reasons for landing
				 * here: a fiber that structurally cannot suspend (a nested
				 * user Fiber, a destructor running under GC) has exactly
				 * the same "the holder can't run" problem as a full
				 * waiter queue -- neither can be turned into a safe block.
				 */
				errno = EWOULDBLOCK;
				return -1;
			}
			fpm_pool_fiber_wait_wake(NULL);
			/* After waking, return to the VERY START of this inner loop — check
			 * the conflict again (several waiters may wake at once, but only one
			 * actually wins). */
		}

		/* Nobody in THIS process holds a conflicting mode: we can try the real
		 * flock(). There is no readiness event for an advisory lock — LOCK_NB
		 * either wins immediately or loses immediately. */
		if (nb) {
			ret = fpm_flock_real(stream, value, ptrparam);
			if (ret == 0) {
				fpm_flock_register_holder(entry, owner, mode);
			}
			return ret;
		}

		ret = fpm_flock_real(stream, mode | LOCK_NB, ptrparam);
		if (ret == 0) {
			fpm_flock_register_holder(entry, owner, mode);
			return 0;
		}
		if (errno != EWOULDBLOCK) {	/* EAGAIN == EWOULDBLOCK on Linux; one check covers both there */
			return ret;	/* real error, not contention — retrying makes no sense */
		}

		/* EWOULDBLOCK: either INTER-PROCESS contention (another process holds the
		 * kernel lock), or — the case fixed here — someone in THIS process won the
		 * real flock() WHILE we previously yielded the processor. Instead of
		 * guessing which one it is, simply return to the top of the "for" loop: if
		 * someone in this process really won, the first thing we do is see that
		 * holder in fpm_flock_conflict() and enter a real suspension instead of
		 * polling/blocking on flock(). That is the entire fix. */
		attempts++;
		if (attempts >= max_attempts || !fpm_pool_fiber_can_wait()) {
			break;
		}
		{
			struct timeval iv;

			iv.tv_sec = FPM_FLOCK_POLL_INTERVAL_USEC / 1000000;
			iv.tv_usec = FPM_FLOCK_POLL_INTERVAL_USEC % 1000000;
			fpm_pool_fiber_wait_wake(&iv);	/* suspend the Fiber without blocking the process; returns on timeout */
		}
	}

	/* Polling attempts exhausted (or polling disabled by
	 * FPMNG_FLOCK_POLL_ATTEMPTS=0, or the Fiber cannot suspend) AND
	 * fpm_flock_conflict() still sees nobody in THIS process at this moment
	 * (checked at the top of the same loop iteration, so this is definitely
	 * INTER-PROCESS contention): the last resort is the real BLOCKING flock().
	 * This suspends the entire OS thread as it does today, but only while ANOTHER
	 * PROCESS holds the lock; by definition that process is running and will
	 * finish in finite time (see E3 in docs/flock-fiber-deadlock-report.md). This
	 * is bounded, not a dead loop, unlike the bug removed by this file (contention
	 * BETWEEN FIBERS IN THE SAME process, where the holder could never regain the
	 * processor). */
	ret = fpm_flock_real(stream, mode, ptrparam);
	if (ret == 0) {
		fpm_flock_register_holder(entry, owner, mode);
	}
	return ret;
}
/* }}} */

void fpm_pool_fiber_flock_install(void) /* {{{ */
{
	if (fpm_flock_installed) {
		return;
	}
	fpm_flock_installed = true;
	fpm_flock_orig_set_option = php_stream_stdio_ops.set_option;
	php_stream_stdio_ops.set_option = fpm_fiber_flock_set_option;
}
/* }}} */
