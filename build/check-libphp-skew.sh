#!/bin/sh
# Fail when a binary prints the patch-level notice of the libphp ABI guard
# (build/libphp/libphp_abi_check.c, issue #220) at start-up (issue #557).
#
#   build/check-libphp-skew.sh <php-fpm-ng binary>
#
# build/ci-package-gate.sh builds in one container and tests in another, and
# each installs the distribution's PHP on its own. When the repository carries
# php85-dev 8.5.10 next to php85-embed 8.5.11 (it did on Alpine edge until
# 2026-10-02), the binary is built against 8.5.10 and runs on libphp 8.5.11,
# prints "built against PHP X, running on libphp Y (patch-level difference,
# supported)" on every start, and about a dozen tests fail on a diff that
# contains only that line. The notice is correct and the tests are right to see
# it: the gate would no longer be testing the packaged binary as a user runs it.
# So the gate stops here, naming both versions and the cause, instead of
# leaving the reader a test diff. Neither the notice nor the tests are changed
# to make the skew pass: a user with such a skew deserves the notice.
#
# The versions come from the notice itself, which is the guard's own statement
# of what it compared; no second source (php-config, package names) can
# disagree with it.
set -eu

BIN=${1:?usage: build/check-libphp-skew.sh <php-fpm-ng binary>}
[ -x "$BIN" ] || { echo "check-libphp-skew.sh: FAIL: $BIN is not executable" >&2; exit 1; }

# -n: no ini file; -v prints the banner and exits. The notice is written by the
# guard before main() runs, so it is on stderr whatever main() then does.
OUT=$("$BIN" -n -v 2>&1 || true)
# No banner means the binary did not start (a FATAL from the guard, a missing
# library): "no notice" would then say nothing about the versions.
echo "$OUT" | grep -q '^PHP [0-9]' || { echo "check-libphp-skew.sh: FAIL: $BIN -v printed no version banner: $OUT" >&2; exit 1; }
NOTICE=$(echo "$OUT" | grep 'php-fpm-ng: notice: built against PHP' || true)
if [ -n "$NOTICE" ]; then
    BUILT=$(echo "$NOTICE" | sed -n 's/.*built against PHP \([^,]*\), running on libphp.*/\1/p')
    LOADED=$(echo "$NOTICE" | sed -n 's/.*running on libphp \([^ ]*\) .*/\1/p')
    cat >&2 <<MSG
check-libphp-skew.sh: FAIL: the binary was built against PHP ${BUILT:-?} and runs on libphp ${LOADED:-?}.
  The build stage and the test stage install PHP from the distribution
  separately, and the repository currently carries a development package
  (php85-dev / php8.5-dev) from another patch release than the embed package
  (php85-embed / libphp8.5-embed). Every start prints
    $NOTICE
  and the tests that compare start-up output fail on it, which is a statement
  about the repository, not about the package. Re-run once both packages are at
  the same version (issue #557).
MSG
    exit 1
fi
echo "check-libphp-skew.sh: ok: $BIN prints no patch-level notice (SDK and libphp agree)"
