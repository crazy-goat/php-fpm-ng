#!/bin/sh
# Issue #531: every fiber IO interception suspends only through the IO seam
# (sapi/fpmng/fpm/fpm_pool_fiber_io.h). This keeps the scheduler's suspension
# primitives private to the seam's backend and keeps libevent out of the
# interception modules and of the php-src patches, so replacing the backend
# (an upstream IO hooks provider) touches one file. Hermetic: grep only.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
FPM="$REPO/sapi/fpmng/fpm"
fail=0

test -f "$FPM/fpm_pool_fiber_io.c"
test -f "$FPM/fpm_pool_fiber_intercept.c"

# 1. The scheduler's suspension primitives: defined in fpm_pool_fiber.c,
#    declared in fpm_pool_fiber.h, called only by fpm_pool_fiber_io.c.
PRIMS='fpm_pool_fiber_(can_wait|wait_fd|waiter|wait_wake|wake|event_base)[[:space:]]*\('
for f in "$FPM"/*.c "$REPO"/patches/*.patch; do
	case "$(basename "$f")" in
		fpm_pool_fiber.c|fpm_pool_fiber_io.c) continue ;;
	esac
	if grep -nE "$PRIMS" "$f" >/dev/null 2>&1; then
		echo "FAIL: $f calls a scheduler primitive directly; go through fpm_fiber_io_run():" >&2
		grep -nE "$PRIMS" "$f" >&2
		fail=1
	fi
done

# 2. No libevent in an interception module or in the patches that compile
#    interception call sites into php-src.
EVPAT='#include[[:space:]]*<event2/|(^|[^a-z_])(event_new|event_add|event_del|event_free|evdns_[a-z_]+|evutil_[a-z_]+)[[:space:]]*\('
for f in "$FPM"/fpm_pool_fiber_xport.c "$FPM"/fpm_pool_fiber_select.c \
	"$FPM"/fpm_pool_fiber_sleep.c "$FPM"/fpm_pool_fiber_flock.c \
	"$FPM"/fpm_pool_fiber_curl.c \
	"$FPM"/fpm_pool_coop_session_patch.c \
	"$FPM"/fpm_pool_fiber_intercept.c \
	"$REPO"/patches/0007-*.patch "$REPO"/patches/0008-*.patch; do
	test -f "$f" || continue	# an entry removed together with its file is fine
	if grep -nE "$EVPAT" "$f" >/dev/null 2>&1; then
		echo "FAIL: $f uses libevent directly; only fpm_pool_fiber_io.c (and the scheduler) may:" >&2
		grep -nE "$EVPAT" "$f" >&2
		fail=1
	fi
done

# 3. The scheduler installs interceptions only through the registry.
if grep -nE 'fpm_pool_fiber_(xport|sleep|flock)_install[[:space:]]*\(' "$FPM/fpm_pool_fiber.c" >/dev/null; then
	echo "FAIL: fpm_pool_fiber.c installs an interception by hand; add it to fpm_pool_fiber_intercept.c" >&2
	fail=1
fi
grep -q 'fpm_fiber_intercept_install_all' "$FPM/fpm_pool_fiber.c" || {
	echo "FAIL: fpm_pool_fiber.c does not call fpm_fiber_intercept_install_all()" >&2
	fail=1
}

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "ok: fiber IO interceptions go through the seam only"
