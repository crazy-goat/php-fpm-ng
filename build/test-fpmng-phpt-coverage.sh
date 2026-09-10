#!/bin/sh
# Regression test for the discovery rules of build/run-fpmng-phpt.sh:
#
#   - issue #95: it refuses to run when a .phpt file this repo owns is not
#     reached by its fpmng-*.phpt glob (cases 1-7);
#   - issue #141: an optional filter runs a subset of the discovered tests,
#     without weakening that refusal (cases 8-13).
#
# Hermetic: it copies the runner into a throwaway tree that plays the part of
# both the repo (the runner locates the owned tests relative to its own path)
# and the prepared php-src, so no build and no binary is needed. The runs stop
# at the preflight -- "NOT MEASURED: TEST_PHP_EXECUTABLE was not supplied" is
# the success marker here, because it means discovery was passed.
set -eu

# The success marker below is the runner's preflight complaining that no binary
# was supplied, so an inherited TEST_PHP_* from a real phpt run in the same
# shell would take these cases past it and into a run of a placeholder .phpt --
# which reports NOT MEASURED and looks like a discovery bug. Measured on
# 192.168.8.50 while implementing issue #141.
unset TEST_PHP_EXECUTABLE TEST_PHP_FPM_EXECUTABLE

REPO=$(cd "$(dirname "$0")/.." && pwd)
RUNNER=$REPO/build/run-fpmng-phpt.sh

WORK=$(mktemp -d "${TMPDIR:-/tmp}/fpmng-phpt-coverage.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

FAKE=$WORK/fake-repo
TESTS=$FAKE/sapi/fpmng/tests
EXCLUDE=$TESTS/not-run-in-ci.list
mkdir -p "$FAKE/build" "$TESTS"
cp "$RUNNER" "$FAKE/build/run-fpmng-phpt.sh"
chmod +x "$FAKE/build/run-fpmng-phpt.sh"

# The prepared tree is the same directory: prepare.sh overlays our tests onto
# the copied upstream ones, so the runner sees the same file names there.
: > "$FAKE/run-tests.php"
: > "$TESTS/tester.inc"
printf '%s\n' '--TEST--' 'placeholder' > "$TESTS/fpmng-placeholder.phpt"

# Extra arguments are the runner's optional filters (issue #141); cases 1-7
# pass none and see the run exactly as CI does.
run_case() {
    rm -rf "$WORK/results"
    ( cd "$FAKE" && ./build/run-fpmng-phpt.sh "$FAKE" "$WORK/results" "$@" ) \
        > "$WORK/out.txt" 2>&1 && status=0 || status=$?
    printf '%s\n' "$status" > "$WORK/status.txt"
}

expect_metadata() {
    expected=$1
    grep -qx "$expected" "$WORK/results/metadata.txt" || {
        printf 'FAIL: %s not reported in metadata.txt\n' "$expected" >&2
        cat "$WORK/results/metadata.txt" >&2
        exit 1
    }
}

expect_output() {
    expected=$1
    if ! grep -qF "$expected" "$WORK/out.txt"; then
        printf 'FAIL: expected output containing: %s\n' "$expected" >&2
        cat "$WORK/out.txt" >&2
        exit 1
    fi
}

# 1. Every owned test matches the glob: discovery passes, the run stops at the
#    preflight for want of a binary.
run_case
expect_output 'NOT MEASURED: TEST_PHP_EXECUTABLE was not supplied'
expect_metadata 'owned_tests=1'
# No filter is not the same as a filter that happens to select everything: the
# metadata has to say which of the two a result directory came from.
expect_metadata 'filter=none'
expect_metadata 'selected_tests=1'

# 2. An owned test the glob cannot reach: this is issue #95 itself, and it must
#    stop the run rather than silently not be tested.
printf '%s\n' '--TEST--' 'unreachable' > "$TESTS/not-prefixed.phpt"
run_case
expect_output 'owned by this repo but were not discovered: not-prefixed.phpt'

# 3. Excluding it is not the answer while it is named that way: build/run-fpm-phpt.sh
#    would take it for an upstream test and run it in the other job.
printf '%s\n' 'not-prefixed.phpt   needs hardware CI does not have' > "$EXCLUDE"
run_case
expect_output 'not-prefixed.phpt must still be named fpmng-*.phpt to be excluded'
rm -f "$TESTS/not-prefixed.phpt" "$EXCLUDE"

# 4. A prefixed test listed with a reason: dropped from the run, and not
#    counted against the discovery.
printf '%s\n' '--TEST--' 'excluded' > "$TESTS/fpmng-excluded.phpt"
printf '%s\n' 'fpmng-excluded.phpt   needs hardware CI does not have' > "$EXCLUDE"
run_case
expect_output 'NOT MEASURED: TEST_PHP_EXECUTABLE was not supplied'
grep -qx 'excluded_tests=1' "$WORK/results/metadata.txt" || {
    printf 'FAIL: excluded_tests=1 not reported in metadata.txt\n' >&2
    exit 1
}
grep -qx 'discovered_tests=1' "$WORK/results/metadata.txt" || {
    printf 'FAIL: the excluded test was not dropped from the run\n' >&2
    cat "$WORK/results/metadata.txt" >&2
    exit 1
}

# 5. The same entry with no newline after it. POSIX read hands the last line
#    back with a non-zero status, and a loop that ignores that would drop the
#    exclusion without a word.
printf '%s' 'fpmng-excluded.phpt   needs hardware CI does not have' > "$EXCLUDE"
run_case
grep -qx 'excluded_tests=1' "$WORK/results/metadata.txt" || {
    printf 'FAIL: an exclusion on an unterminated last line was ignored\n' >&2
    cat "$WORK/results/metadata.txt" >&2
    exit 1
}

# 6. A bare name with no reason is not an exclusion.
printf '%s\n' 'fpmng-excluded.phpt' > "$EXCLUDE"
run_case
expect_output 'no reason given for fpmng-excluded.phpt'

# 7. An exclusion whose file is gone: a stale entry is a failure, not a no-op.
rm -f "$TESTS/fpmng-excluded.phpt"
printf '%s\n' 'fpmng-excluded.phpt   needs hardware CI does not have' > "$EXCLUDE"
run_case
expect_output 'no such test: fpmng-excluded.phpt'

# --- issue #141: running a subset through the same harness ---------------
rm -f "$EXCLUDE"
printf '%s\n' '--TEST--' 'second' > "$TESTS/fpmng-second.phpt"

# 8. A filter naming one test: only that one is selected, and the discovery is
#    still the full owned set, so the count in metadata.txt distinguishes "ran
#    one test" from "owns one test".
run_case fpmng-second.phpt
expect_output 'NOT MEASURED: TEST_PHP_EXECUTABLE was not supplied'
expect_metadata 'discovered_tests=2'
expect_metadata 'selected_tests=1'
expect_metadata 'filter=fpmng-second.phpt'
grep -q 'fpmng-second.phpt' "$WORK/results/selected.tsv" || {
    printf 'FAIL: the filtered test is not in selected.tsv\n' >&2
    cat "$WORK/results/selected.tsv" >&2
    exit 1
}
# The result table is the record of what ran, so the test that was filtered out
# must not appear in it at all -- not even as NOT MEASURED.
if grep -q 'fpmng-placeholder.phpt' "$WORK/results/results.tsv"; then
    printf 'FAIL: a test the filter excluded is still in results.tsv\n' >&2
    cat "$WORK/results/results.tsv" >&2
    exit 1
fi

# 9. A plain word is a substring, so no one has to type the whole file name.
run_case second
expect_metadata 'selected_tests=1'

# 10. A glob reaching both, and both reached twice over (the substring arm
#     matches as well): the selection is deduplicated, or run-tests.php would
#     run a test twice and only the last status would survive.
run_case 'fpmng-*.phpt' second
expect_metadata 'selected_tests=2'

# 11. A filter that matches nothing is a typo. Running the rest of the suite
#     instead would report a green run for a test that never executed.
run_case fpmng-thrid
expect_output 'filter matched none of the 2 discovered tests: fpmng-thrid'

# 12. A filter cannot resurrect an excluded test: not-run-in-ci.list is
#     subtracted before the filter is applied.
printf '%s\n' 'fpmng-second.phpt   needs hardware CI does not have' > "$EXCLUDE"
run_case second
expect_output 'filter matched none of the 1 discovered tests: second'
rm -f "$EXCLUDE"

# 13. The coverage check of case 2 is not weakened by a filter: an owned test
#     the glob cannot reach still stops the run, however narrow the request.
printf '%s\n' '--TEST--' 'unreachable' > "$TESTS/not-prefixed.phpt"
run_case fpmng-second.phpt
expect_output 'owned by this repo but were not discovered: not-prefixed.phpt'
rm -f "$TESTS/not-prefixed.phpt" "$TESTS/fpmng-second.phpt"

printf 'test-fpmng-phpt-coverage.sh: 13/13 cases passed\n'
