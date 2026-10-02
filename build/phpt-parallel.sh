#!/bin/sh
# Make a .phpt test tree safe for `run-tests.php -j` (issue #394).
#
#   build/phpt-parallel.sh <tree>
#
# <tree> is the directory with run-tests.php and sapi/fpmng/tests/, either one
# build/phpt-tree.sh assembled or one build/prepare.sh left behind. It is a
# throwaway copy; the pinned bundle under third_party/php-src/ is never touched
# (build/phpt-fixture-patches/README.md says why). Two changes, both idempotent:
#
#   1. build/phpt-fixture-patches/*.patch: run-tests.php tells each test which
#      worker runs it, and tester.inc allocates ports from that worker's block;
#      run-tests.php no longer retries a test whose output says "address already
#      in use", so a port collision fails instead of passing as WARN (#562).
#   2. sapi/fpmng/tests/CONFLICTS is removed. Upstream ships it with the single
#      word "all" (spurious failures on Azure), and run-tests.php then pulls
#      every test of that directory out of the parallel pool and runs them one
#      after another at the end -- which would make -j a no-op for the whole
#      suite. A test that really cannot share the machine says so itself in a
#      --CONFLICTS-- section.
#   3. Tests that start a gateway pool get a --CONFLICTS-- section of their own
#      (see below), because every gateway binds the operator listener's default
#      address, 127.0.0.1:9253.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
TREE=${1:?usage: build/phpt-parallel.sh <tree>}
TESTS=$TREE/sapi/fpmng/tests

fail() { echo "phpt-parallel.sh: FAIL: $*" >&2; exit 1; }

[ -f "$TESTS/tester.inc" ] || fail "no tester.inc below $TESTS"
[ -f "$TREE/run-tests.php" ] || fail "no run-tests.php below $TREE"

# The patches are written against the tree's own layout (the harness lives in
# sapi/fpmng/tests/ there, not in upstream's sapi/fpm/tests/). Each carries an
# issue marker in the file it changes, which is how a patched file is told from
# a pristine one: patch --reverse --dry-run cannot, because BSD patch answers
# its own "previously applied" prompt with yes. The marker is checked per patch,
# so a tree patched by an earlier version of this script (only 0001 and 0002)
# picks up the patches added since.
apply_patch() { # <patch file> <file it changes, relative to the tree> <marker>
    grep -q "$3" "$TREE/$2" && return 0
    patch -d "$TREE" -p1 --forward --silent --no-backup-if-mismatch < "$REPO/build/phpt-fixture-patches/$1" >&2 ||
        fail "$1 does not apply to $TREE"
}
apply_patch 0001-tester-port-base-per-worker.patch sapi/fpmng/tests/tester.inc 'Issue #394 (php-fpm-ng)'
apply_patch 0002-run-tests-worker-env-for-tests.patch run-tests.php 'Issue #394 (php-fpm-ng)'
apply_patch 0003-run-tests-no-retry-on-port-collision.patch run-tests.php 'Issue #562 (php-fpm-ng)'
grep -q 'Issue #394 (php-fpm-ng)' "$TESTS/tester.inc" || fail "tester.inc has no TEST_PHP_WORKER after patching"
grep -q 'Issue #394 (php-fpm-ng)' "$TREE/run-tests.php" || fail "run-tests.php does not hand TEST_PHP_WORKER to the tests after patching"
grep -q 'Issue #562 (php-fpm-ng)' "$TREE/run-tests.php" || fail "run-tests.php still retries a test on 'address already in use' after patching"

rm -f "$TESTS/CONFLICTS"

# A pool.type = gateway opens the operator endpoint by default, on the fixed
# address 127.0.0.1:9253 (docs/operator-endpoint.md), whatever port the test's
# own listen got from the Tester. Two gateways in one master share it (the
# second drops its pages, issue #388), but two masters in two workers do not:
# the second fails with "unable to bind listening socket ... 9253" and the test
# reports a startup error that no port block can prevent. The tests that start a
# gateway therefore share one conflict key, and run-tests.php never runs two of
# them at once; everything else still runs in parallel. Found by looking at the
# first -j runs of the suite, not by guessing (issue #394).
#
# The section is added to the tree's copy rather than to the .phpt files so that
# a new gateway test cannot forget it. Detection is the text: the pool type in a
# configuration, or the skip guard that names it for a test whose configuration
# comes from an include (fpmng-raw-upstream.inc), or an http.route[] directive,
# which only a gateway reads.
for f in "$TESTS"/fpmng-*.phpt; do
    grep -q '^--CONFLICTS--' "$f" && continue
    grep -Eq "pool\\.type[[:space:]]*=[[:space:]]*gateway|unsupported\\('gateway'\\)|http\\.route\\[" "$f" || continue
    awk '!done && /^--FILE--/ { print "--CONFLICTS--"; print "operator-default-listener"; done = 1 } { print }' "$f" > "$f.tmp" &&
        mv "$f.tmp" "$f"
done
