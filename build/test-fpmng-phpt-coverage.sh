#!/bin/sh
# Regression test for issue #95: build/run-fpmng-phpt.sh refuses to run when a
# .phpt file this repo owns is not reached by its fpmng-*.phpt glob.
#
# Hermetic: it copies the runner into a throwaway tree that plays the part of
# both the repo (the runner locates the owned tests relative to its own path)
# and the prepared php-src, so no build and no binary is needed. The runs stop
# at the preflight -- "NOT MEASURED: TEST_PHP_EXECUTABLE was not supplied" is
# the success marker here, because it means discovery was passed.
set -eu

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

run_case() {
    rm -rf "$WORK/results"
    ( cd "$FAKE" && ./build/run-fpmng-phpt.sh "$FAKE" "$WORK/results" ) \
        > "$WORK/out.txt" 2>&1 && status=0 || status=$?
    printf '%s\n' "$status" > "$WORK/status.txt"
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
grep -qx 'owned_tests=1' "$WORK/results/metadata.txt" || {
    printf 'FAIL: owned_tests=1 not reported in metadata.txt\n' >&2
    cat "$WORK/results/metadata.txt" >&2
    exit 1
}

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

printf 'test-fpmng-phpt-coverage.sh: 7/7 cases passed\n'
