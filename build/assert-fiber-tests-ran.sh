#!/bin/sh
# Issue #97: assert that the fiber-gated .phpt tests actually RAN.
#
# The point of the --enable-fpmng-fiber CI cell is not that it is green -- a
# cell where all ten of these tests SKIP is green too, and that was the state
# this issue was filed about: ten owned tests that could only ever report
# SKIP, so nothing in CI would have noticed the fiber executor's request
# isolation, its flock() interception, or the listening-flag normalisation
# breaking.
#
# So the job's real assertion lives here rather than in the runner's exit
# status: every test below must have a status that is not SKIP. A build flag
# that silently stops taking effect, a --SKIPIF-- guard that starts matching
# for a new reason, or a renamed test file all turn this red instead of
# quietly restoring the hole.
#
# Usage: assert-fiber-tests-ran.sh <results-dir>
set -eu

RESULTS=${1:?usage: assert-fiber-tests-ran.sh <results-dir>}
TSV="$RESULTS/results.tsv"

[ -f "$TSV" ] || {
    echo "assert-fiber-tests-ran.sh: no $TSV -- the suite did not get far enough to produce per-test results" >&2
    exit 2
}

# The ten named in issue #97, all gated on "php-fpm-ng was not built with
# --enable-fpmng-fiber" or on an extension this cell's configure line
# provides. Listed literally, not globbed on 'fiber': two of them
# (fpmng-reload-listening-flags, fpmng-unrelated-listening-flags) do not carry
# the word, and a glob would also silently shrink to nothing if the files were
# renamed -- which is one of the regressions this is here to catch.
TESTS="fpmng-fiber-dropped-request.phpt
fpmng-fiber-exceptions.phpt
fpmng-fiber-flock.phpt
fpmng-fiber-request-isolation.phpt
fpmng-fiber-sleep-concurrency.phpt
fpmng-fiber-stream-select.phpt
fpmng-fiber-tls-concurrency.phpt
fpmng-pool-type-fiber-matrix.phpt
fpmng-reload-listening-flags.phpt
fpmng-unrelated-listening-flags.phpt"

rc=0
for t in $TESTS; do
    line=$(awk -F'\t' -v name="$t" '$1 ~ ("/" name "$") { print; exit }' "$TSV" || true)
    if [ -z "$line" ]; then
        echo "FAIL: $t is not in $TSV at all -- renamed, deleted, or never discovered" >&2
        rc=1
        continue
    fi
    status=$(printf '%s\n' "$line" | cut -f2)
    # A whitelist, not "anything but SKIP". SKIP is the status this issue was
    # filed about, but it is not the only one that means "this test did not
    # tell us anything": build/run-fpmng-phpt.sh buckets a test that
    # run-tests.php emitted no status line for as NOT MEASURED, sets
    # measurement_status=PARTIAL, and still exits with run-tests.php's own
    # status -- which is 0, because that file only fails on
    # FAILED/BORKED/LEAKED. WARN is excluded there too. So every one of those
    # would have reached this script and been printed as "ok" while measuring
    # nothing, which is the same hole in a different colour.
    case "$status" in
    PASS)
        ;;
    SKIP)
        echo "FAIL: $t reported SKIP on the --enable-fpmng-fiber cell, which is the whole reason this cell exists" >&2
        rc=1
        continue
        ;;
    *)
        echo "FAIL: $t reported '$status' on the --enable-fpmng-fiber cell; only PASS means this test actually ran and checked something" >&2
        rc=1
        continue
        ;;
    esac
    echo "ok: $t -> $status"
done

[ "$rc" -eq 0 ] || {
    echo "assert-fiber-tests-ran.sh: the fiber cell did not actually exercise the fiber tests" >&2
    exit 1
}
echo "assert-fiber-tests-ran.sh: all 10 fiber-gated tests ran on this cell"
