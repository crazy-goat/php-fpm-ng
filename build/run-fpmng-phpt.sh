#!/bin/sh
# Run php-fpm-ng's own fpmng-*.phpt regression tests (task 003).
#
# Usage:
#   TEST_PHP_EXECUTABLE=/path/to/php \
#   TEST_PHP_FPM_EXECUTABLE=/path/to/php-fpm-ng \
#   build/run-fpmng-phpt.sh /path/to/prepared-php-src /path/to/results [filter...]
#
# A filter selects a subset of the discovered tests for the run, so a single
# test can be debugged through this runner instead of by hand. It must be this
# runner: FPM\Tester::findExecutable() ignores TEST_PHP_FPM_EXECUTABLE and
# looks for a binary named php-fpm two levels above TEST_PHP_EXECUTABLE, so a
# run-tests.php invoked directly on one .phpt SKIPs with "php-fpm binary not
# found" until something builds the symlink harness below (issue #141).
#
# The prepared tree must contain sapi/fpmng/tests/ with tester.inc from
# build/prepare.sh. Only files matching fpmng-*.phpt are executed — upstream's
# copied suite is intentionally excluded and is run by build/run-fpm-phpt.sh.
#
# The glob is not the definition of ownership, only its naming convention. Every
# .phpt file in this repo's sapi/fpmng/tests/ is ours, and the run is checked
# against that directory: a file this repo owns that the glob does not reach
# fails the run instead of quietly not being tested (issue #95, where nine
# non-prefixed files were reachable only because build/run-fpm-phpt.sh sweeps
# the whole directory). A test that must not run in CI goes into
# sapi/fpmng/tests/not-run-in-ci.list with a reason.
set -eu

usage() {
    cat >&2 <<'EOF'
Usage: TEST_PHP_EXECUTABLE=/path/to/php \
       TEST_PHP_FPM_EXECUTABLE=/path/to/php-fpm-ng \
       build/run-fpmng-phpt.sh /path/to/prepared-php-src /path/to/results [filter...]

Each optional filter selects tests to run out of the discovered set: a shell
glob or a plain substring, matched against the test file name. Every filter
must match at least one discovered test. With no filter the whole owned suite
runs.

Both executable paths are required. The prepared source tree must contain
sapi/fpmng/tests/tester.inc from build/prepare.sh. TEST_FPM_EXTENSION_DIR and
TEST_FPM_RUN_AS_ROOT are passed through when set. TEST_FPM_TIMEOUT defaults to
120 seconds because fpmng-cron-schedule.phpt may wait for the next minute tick.
EOF
    exit 2
}

fail() {
    printf '%s\n' "run-fpmng-phpt.sh: $*" >&2
    exit 1
}

[ "$#" -ge 2 ] || usage

PHPSRC_INPUT=$1
RESULTS_INPUT=$2
shift 2
TEST_DIR=sapi/fpmng/tests
REPO=$(cd "$(dirname "$0")/.." && pwd)
OWNED_DIR=$REPO/$TEST_DIR
EXCLUDE_LIST=$OWNED_DIR/not-run-in-ci.list
OWNED_COUNT=unknown
EXCLUDED_COUNT=0
SELECTED_COUNT=0
FILTERED=no
FILTER_DESC=none

CLI_BIN_INPUT=${TEST_PHP_EXECUTABLE-}
FPM_BIN_INPUT=${TEST_PHP_FPM_EXECUTABLE-}
CLI_BIN=not-supplied
FPM_BIN=not-supplied
CLI_SHA=not-measured
FPM_SHA=not-measured
CLI_VERSION=not-measured
FPM_VERSION=not-measured
MARKERS=not-measured
SOURCE_COMMIT=unknown
HARNESS_DIR=

resolve_path() {
    input=$1
    case "$input" in
        /*) absolute=$input ;;
        *) absolute=$PWD/$input ;;
    esac

    if command -v realpath >/dev/null 2>&1; then
        realpath "$absolute"
        return
    fi

    (cd "$(dirname "$absolute")" && printf '%s/%s\n' "$PWD" "$(basename "$absolute")")
}

sha256_file() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        return 1
    fi
}

cleanup() {
    status=$?
    if [ -n "$HARNESS_DIR" ]; then
        rm -rf "$HARNESS_DIR"
    fi
    exit "$status"
}
trap cleanup EXIT

[ -d "$PHPSRC_INPUT" ] || fail "PHP source directory does not exist: $PHPSRC_INPUT"
[ -f "$PHPSRC_INPUT/run-tests.php" ] || fail "run-tests.php not found below: $PHPSRC_INPUT"
[ -d "$PHPSRC_INPUT/$TEST_DIR" ] || fail "copied upstream FPM tests not found: $PHPSRC_INPUT/$TEST_DIR"
[ -f "$PHPSRC_INPUT/$TEST_DIR/tester.inc" ] || fail "tester.inc not found below: $PHPSRC_INPUT/$TEST_DIR"

PHPSRC=$(resolve_path "$PHPSRC_INPUT") || fail "cannot resolve PHP source directory: $PHPSRC_INPUT"
mkdir -p "$RESULTS_INPUT" || fail "cannot create results directory: $RESULTS_INPUT"
RESULTS_DIR=$(resolve_path "$RESULTS_INPUT") || fail "cannot resolve results directory: $RESULTS_INPUT"

DISCOVERED=$RESULTS_DIR/discovered.tsv
SELECTED=$RESULTS_DIR/selected.tsv
STATUS_RAW=$RESULTS_DIR/statuses.raw.tsv
RESULTS=$RESULTS_DIR/results.tsv
FAILED=$RESULTS_DIR/failed.raw.txt
RUN_LOG=$RESULTS_DIR/run.log
OUTPUT_LOG=$RESULTS_DIR/test-output.log
METADATA=$RESULTS_DIR/metadata.txt
SUMMARY=$RESULTS_DIR/summary.txt
SLOW=$RESULTS_DIR/slow.tsv

rm -f "$DISCOVERED" "$SELECTED" "$STATUS_RAW" "$RESULTS" "$FAILED" "$RUN_LOG" "$OUTPUT_LOG" "$METADATA" "$SUMMARY" "$SLOW"

if ! (
    cd "$PHPSRC"
    find "$TEST_DIR" -maxdepth 1 -type f -name 'fpmng-*.phpt' -print | LC_ALL=C sort
) > "$DISCOVERED"; then
    fail "cannot enumerate fpmng-owned .phpt tests"
fi
TEST_COUNT=$(awk 'END {print NR + 0}' "$DISCOVERED")
[ "$TEST_COUNT" -gt 0 ] || fail "no fpmng-*.phpt tests were discovered"

# Coverage check: what this repo owns against what the glob reached. Skipped,
# with owned_tests=unknown in the summary, when the script runs detached from a
# checkout (only then is the owned set genuinely unknowable — the prepared tree
# mixes our tests with upstream's copied ones and nothing in a .phpt says which
# is which).
if [ -d "$OWNED_DIR" ]; then
    OWNED_ALL=$RESULTS_DIR/owned.all.txt
    OWNED_EXCLUDED=$RESULTS_DIR/owned.excluded.txt
    OWNED_EXPECTED=$RESULTS_DIR/owned.expected.txt
    DISCOVERED_NAMES=$RESULTS_DIR/discovered.names.txt

    (cd "$OWNED_DIR" && find . -maxdepth 1 -type f -name '*.phpt' -exec basename {} \; ) \
        | LC_ALL=C sort > "$OWNED_ALL"

    : > "$OWNED_EXCLUDED"
    if [ -f "$EXCLUDE_LIST" ]; then
        # "<name>.phpt <reason>" — a name with no reason is rejected, so an
        # exclusion cannot outlive the sentence that justified it.
        #
        # `|| [ -n "$line" ]`: read returns non-zero on a last line with no
        # newline after it, and the loop body would never see that entry. An
        # exclusion that silently does not apply is the failure mode this whole
        # check exists to remove.
        while IFS= read -r line || [ -n "$line" ]; do
            case "$line" in
                ''|'#'*) continue ;;
            esac
            name=${line%% *}
            reason=${line#"$name"}
            reason=$(printf '%s' "$reason" | sed 's/^[[:space:]]*//')
            [ -n "$reason" ] || fail "$EXCLUDE_LIST: no reason given for $name"
            [ -f "$OWNED_DIR/$name" ] || fail "$EXCLUDE_LIST: no such test: $name"
            # An excluded test still has to carry the prefix: build/run-fpm-phpt.sh
            # takes "not named fpmng-*" to mean "upstream's", so a non-prefixed
            # file would be skipped here and then run by that job instead, its
            # failure filed under upstream's suite.
            case "$name" in
                fpmng-*.phpt) ;;
                *) fail "$EXCLUDE_LIST: $name must still be named fpmng-*.phpt to be excluded" ;;
            esac
            printf '%s\n' "$name" >> "$OWNED_EXCLUDED"
        done < "$EXCLUDE_LIST"
    fi
    LC_ALL=C sort -o "$OWNED_EXCLUDED" "$OWNED_EXCLUDED"
    LC_ALL=C comm -23 "$OWNED_ALL" "$OWNED_EXCLUDED" > "$OWNED_EXPECTED"

    # An excluded test keeps its fpmng- prefix, so the glob still finds it; drop
    # it here rather than asking anyone to rename a file to stop running it.
    if [ -s "$OWNED_EXCLUDED" ]; then
        awk 'NR == FNR { excluded[$0] = 1; next }
             { name = $0; sub(/.*\//, "", name); if (!(name in excluded)) print }' \
            "$OWNED_EXCLUDED" "$DISCOVERED" > "$DISCOVERED.filtered"
        mv "$DISCOVERED.filtered" "$DISCOVERED"
        TEST_COUNT=$(awk 'END {print NR + 0}' "$DISCOVERED")
    fi

    sed 's|.*/||' "$DISCOVERED" | LC_ALL=C sort > "$DISCOVERED_NAMES"
    MISSING=$(LC_ALL=C comm -23 "$OWNED_EXPECTED" "$DISCOVERED_NAMES" | tr '\n' ' ')
    if [ -n "$MISSING" ]; then
        fail "these tests are owned by this repo but were not discovered: ${MISSING}(rename them to fpmng-*.phpt; a test that must not run in CI keeps the prefix and goes into $EXCLUDE_LIST with a reason)"
    fi
    OWNED_COUNT=$(awk 'END {print NR + 0}' "$OWNED_ALL")
    EXCLUDED_COUNT=$(awk 'END {print NR + 0}' "$OWNED_EXCLUDED")
    EXPECTED_COUNT=$(awk 'END {print NR + 0}' "$OWNED_EXPECTED")
    # Not implied by the check above: the prepared tree can also be missing a
    # test that the repo has, or hold a stale fpmng-*.phpt from an older copy.
    [ "$TEST_COUNT" -eq "$EXPECTED_COUNT" ] || fail "discovered $TEST_COUNT tests in $PHPSRC/$TEST_DIR but this repo owns $EXPECTED_COUNT runnable ones; re-run build/prepare.sh against a clean tree"
fi

# The filter narrows what runs, never what is owned: it is applied after the
# coverage check above, so `run-fpmng-phpt.sh <src> <results> cron` still fails
# a repo whose glob misses an owned test (issue #95). The other half of that
# separation is that a filter cannot select an excluded test -- by this point
# not-run-in-ci.list has already been subtracted from $DISCOVERED.
if [ "$#" -gt 0 ]; then
    FILTERED=yes
    FILTER_DESC=$*
    : > "$SELECTED"
    for pattern in "$@"; do
        [ -n "$pattern" ] || fail 'an empty filter selects nothing; drop it or pass "*"'
        matched=0
        while IFS= read -r discovered_test; do
            discovered_name=${discovered_test##*/}
            # Unquoted on the left so a glob stays a glob; quoted on the right
            # so a plain word is a substring. "cron", "fpmng-cron-*.phpt" and
            # the full file name therefore all work, and none of them needs
            # quoting against the caller's own shell.
            # shellcheck disable=SC2254
            case "$discovered_name" in
                $pattern|*"$pattern"*)
                    matched=1
                    printf '%s\n' "$discovered_test" >> "$SELECTED"
                    ;;
            esac
        done < "$DISCOVERED"
        # A filter that matches nothing is a typo, and running the remaining
        # patterns instead would report a green suite for tests nobody asked
        # for.
        [ "$matched" -eq 1 ] || fail "filter matched none of the $TEST_COUNT discovered tests: $pattern (names are in $DISCOVERED)"
    done
    # Two filters may reach the same test; run-tests.php would run it twice and
    # the second status would overwrite the first in $STATUS_RAW.
    LC_ALL=C sort -u -o "$SELECTED" "$SELECTED"
else
    cp "$DISCOVERED" "$SELECTED"
fi
SELECTED_COUNT=$(awk 'END {print NR + 0}' "$SELECTED")
[ "$SELECTED_COUNT" -gt 0 ] || fail 'no test was selected to run'

if [ -e "$PHPSRC/.git" ] && command -v git >/dev/null 2>&1; then
    SOURCE_COMMIT=$(git -C "$PHPSRC" rev-parse HEAD 2>/dev/null || printf '%s' unknown)
fi

write_metadata() {
    {
        printf '%s\n' 'runner=build/run-fpmng-phpt.sh'
        printf '%s\n' "php_src=$PHPSRC"
        printf '%s\n' "php_src_commit=$SOURCE_COMMIT"
        printf '%s\n' "test_directory=$TEST_DIR"
        printf '%s\n' "discovered_tests=$TEST_COUNT"
        printf '%s\n' "owned_tests=$OWNED_COUNT"
        printf '%s\n' "excluded_tests=$EXCLUDED_COUNT"
        printf '%s\n' "filter=$FILTER_DESC"
        printf '%s\n' "selected_tests=$SELECTED_COUNT"
        printf '%s\n' "requested_php_cli=${CLI_BIN_INPUT:-not-supplied}"
        printf '%s\n' "php_cli=$CLI_BIN"
        printf '%s\n' "php_cli_sha256=$CLI_SHA"
        printf '%s\n' 'php_cli_version_begin'
        printf '%s\n' "$CLI_VERSION"
        printf '%s\n' 'php_cli_version_end'
        printf '%s\n' "requested_php_fpm_ng=${FPM_BIN_INPUT:-not-supplied}"
        printf '%s\n' "php_fpm_ng=$FPM_BIN"
        printf '%s\n' "php_fpm_ng_sha256=$FPM_SHA"
        printf '%s\n' 'php_fpm_ng_version_begin'
        printf '%s\n' "$FPM_VERSION"
        printf '%s\n' 'php_fpm_ng_version_end'
        printf '%s\n' 'php_fpm_ng_strings_begin'
        printf '%s\n' "$MARKERS"
        printf '%s\n' 'php_fpm_ng_strings_end'
        printf '%s\n' "TEST_PHP_EXECUTABLE=$CLI_BIN"
        printf '%s\n' "TEST_PHP_FPM_EXECUTABLE=${HARNESS_FPM-unset}"
        printf '%s\n' "TEST_FPM_EXTENSION_DIR=${TEST_FPM_EXTENSION_DIR-unset}"
        printf '%s\n' "TEST_FPM_RUN_AS_ROOT=${TEST_FPM_RUN_AS_ROOT-unset}"
        printf '%s\n' "TEST_FPM_TIMEOUT=${TEST_FPM_TIMEOUT-120}"
        printf '%s\n' "TEST_FPM_MIN_PASS=${TEST_FPM_MIN_PASS-unset}"
    } > "$METADATA"
}

write_counts() {
    awk -F '\t' '
        NR > 1 { count[$2]++ }
        END {
            printf "PASS=%d\n", count["PASS"] + 0
            printf "FAIL/ERROR=%d\n", count["FAIL/ERROR"] + 0
            printf "SKIP=%d\n", count["SKIP"] + 0
            printf "WARN=%d\n", count["WARN"] + 0
            printf "NOT MEASURED=%d\n", count["NOT MEASURED"] + 0
            printf "TOTAL=%d\n", (count["PASS"] + count["FAIL/ERROR"] + count["SKIP"] + count["WARN"] + count["NOT MEASURED"]) + 0
        }
    ' "$RESULTS"
}

write_not_measured() {
    blocker=$1
    : > "$STATUS_RAW"
    : > "$FAILED"
    : > "$OUTPUT_LOG"
    printf 'NOT MEASURED: %s\n' "$blocker" > "$RUN_LOG"
    {
        printf 'test\tcategory\traw_status\n'
        while IFS= read -r test; do
            printf '%s\tNOT MEASURED\tNOT_MEASURED\n' "$test"
        done < "$SELECTED"
    } > "$RESULTS"
    {
        printf '%s\n' 'measurement_status=NOT MEASURED'
        printf '%s\n' 'run_exit_status=NOT_RUN'
        printf 'blocker=%s\n' "$blocker"
        write_counts
    } > "$SUMMARY"
    write_metadata
    printf '%s\n' "run-fpmng-phpt.sh: NOT MEASURED: $blocker" >&2
}

preflight_fail() {
    write_not_measured "$1"
    exit 1
}

[ -n "$CLI_BIN_INPUT" ] || preflight_fail 'TEST_PHP_EXECUTABLE was not supplied'
[ -n "$FPM_BIN_INPUT" ] || preflight_fail 'TEST_PHP_FPM_EXECUTABLE was not supplied'
[ -x "$CLI_BIN_INPUT" ] || preflight_fail "TEST_PHP_EXECUTABLE is not executable: $CLI_BIN_INPUT"
[ -x "$FPM_BIN_INPUT" ] || preflight_fail "TEST_PHP_FPM_EXECUTABLE is not executable: $FPM_BIN_INPUT"

CLI_BIN=$(resolve_path "$CLI_BIN_INPUT") || preflight_fail "cannot resolve TEST_PHP_EXECUTABLE: $CLI_BIN_INPUT"
FPM_BIN=$(resolve_path "$FPM_BIN_INPUT") || preflight_fail "cannot resolve TEST_PHP_FPM_EXECUTABLE: $FPM_BIN_INPUT"
[ -x "$CLI_BIN" ] || preflight_fail "resolved TEST_PHP_EXECUTABLE is not executable: $CLI_BIN"
[ -x "$FPM_BIN" ] || preflight_fail "resolved TEST_PHP_FPM_EXECUTABLE is not executable: $FPM_BIN"

if ! CLI_SHA=$(sha256_file "$CLI_BIN"); then
    preflight_fail "cannot calculate SHA-256 for $CLI_BIN"
fi
if ! FPM_SHA=$(sha256_file "$FPM_BIN"); then
    preflight_fail "cannot calculate SHA-256 for $FPM_BIN"
fi
if ! CLI_VERSION=$("$CLI_BIN" -v 2>&1); then
    preflight_fail "TEST_PHP_EXECUTABLE failed to report its version: $CLI_BIN"
fi
if ! FPM_VERSION=$("$FPM_BIN" -v 2>&1); then
    preflight_fail "TEST_PHP_FPM_EXECUTABLE failed to report its version: $FPM_BIN"
fi

command -v strings >/dev/null 2>&1 || preflight_fail 'strings is required to verify the FPM binary'
if ! FPM_STRINGS=$(strings "$FPM_BIN" 2>/dev/null); then
    preflight_fail "cannot inspect strings in $FPM_BIN"
fi
MARKERS=$(printf '%s\n' "$FPM_STRINGS" | grep -E 'php-fpm-ng|pool\.type|fpmng_' | LC_ALL=C sort -u || true)
printf '%s\n' "$MARKERS" | grep -q 'fpmng_' || preflight_fail "strings has no fpmng_ marker: $FPM_BIN"
printf '%s\n' "$MARKERS" | grep -Eq 'php-fpm-ng|pool\.type' || preflight_fail "strings has no pool.type/php-fpm-ng marker: $FPM_BIN"

HARNESS_DIR=$RESULTS_DIR/.harness
mkdir -p "$HARNESS_DIR/bin" "$HARNESS_DIR/fpm"
HARNESS_FPM=$HARNESS_DIR/bin/php-fpm-ng
ln -s "$FPM_BIN" "$HARNESS_FPM"
ln -s "$FPM_BIN" "$HARNESS_DIR/fpm/php-fpm"
# tester.inc::findExecutable() (as shipped in the php-8.5.9 tag; the newer
# dev snapshot task 001 was validated against reads TEST_PHP_FPM_EXECUTABLE
# directly, but that isn't upstream API to rely on) never looks at
# TEST_PHP_FPM_EXECUTABLE. It strips two path components off
# TEST_PHP_EXECUTABLE and checks "<that>/fpm/php-fpm" — so TEST_PHP_EXECUTABLE
# must itself live two levels under a directory containing fpm/php-fpm, i.e.
# under $HARNESS_DIR, not at the caller-supplied $CLI_BIN path.
HARNESS_CLI=$HARNESS_DIR/bin/php
ln -s "$CLI_BIN" "$HARNESS_CLI"
write_metadata

TIMEOUT=${TEST_FPM_TIMEOUT-120}
case "$TIMEOUT" in
    ''|*[!0-9]*) preflight_fail "TEST_FPM_TIMEOUT must be a positive integer: $TIMEOUT" ;;
    0) preflight_fail 'TEST_FPM_TIMEOUT must be greater than zero' ;;
esac

# Per-test durations (--show-slow, run-tests.php:116). The suite is serial (no
# -j: upstream's Tester::getPort() bases every instance at the same port, see
# issue #394), so its wall clock is the sum of its tests and a handful of them
# dominate it -- without this the log says only how long all 124 took together.
# 1000 ms rather than 0: at 0 every test is "slow" and the table stops ranking
# anything. Set TEST_FPM_SHOW_SLOW_MS=0 to turn the table off entirely.
SHOW_SLOW_MS=${TEST_FPM_SHOW_SLOW_MS-1000}
case "$SHOW_SLOW_MS" in
    ''|*[!0-9]*) preflight_fail "TEST_FPM_SHOW_SLOW_MS must be a non-negative integer: $SHOW_SLOW_MS" ;;
esac

# The floor on PASS, and why this runner needs one at all.
#
# The standing trap in this repository is a run that skips every test and still
# reports success: as root php-fpm refuses to start, so every test SKIPs and
# run-tests.php exits 0. Nothing above catches it -- the ownership check of
# issue #95 compares discovered against owned, which is a property of the tree
# and is just as true when nothing ran.
#
# Until 2026-09-17 the defence was build/ci-package-gate.sh, which asserts the
# exact PASS/SKIP/TOTAL counts per flavour and says so in its own header. That
# gate left the pull-request path with issue #393 (it now runs only in
# release.yml, on the tag), which took the defence with it. This is its
# replacement on the fast path.
#
# A FRACTION, NOT THE EXACT COUNT. The gate can afford exact numbers because a
# package flavour is a fixed configuration; it pays for them with a long block
# of comments that every new test has to update. A runner used by developers on
# arbitrary builds cannot: how many tests legitimately skip depends on which
# --enable-fpmng-* flags the binary carries. So the assertion is "at least half
# of the selected tests passed", which needs no maintenance and still catches
# PASS=0.
#
# Half is not arbitrary. The worst legitimate configuration is the plain apk
# package -- no TLS, no ACME, no fiber -- and release run 35200538713 measured
# it at PASS=62 SKIP=43 of 105, while the gate's current expectation for it is
# PASS=80 of 124. Half of 124 is 62, so the tightest real configuration clears
# this by 18 tests. Set TEST_FPM_MIN_PASS to an explicit count to override, or
# to 0 to turn the check off.
MIN_PASS=${TEST_FPM_MIN_PASS-}
case "$MIN_PASS" in
    '') ;;
    *[!0-9]*) preflight_fail "TEST_FPM_MIN_PASS must be a non-negative integer: $MIN_PASS" ;;
esac
# Unquoted on purpose below (word splitting is what turns this into two argv
# entries); empty when the table is off, and run-tests.php then never sees the
# flag -- passing --show-slow 0 would mark every test slow instead.
SHOW_SLOW_ARGS=
if [ "$SHOW_SLOW_MS" -gt 0 ]; then
    SHOW_SLOW_ARGS="--show-slow $SHOW_SLOW_MS"
fi

# On $FILTERED, not on "$FILTER_DESC" = none: "none" is a legal filter word, and
# a filtered run must never describe itself as a full one. (It would die at the
# no-match check above today, since no test name contains "none" -- but that is
# a property of the current test names, not something to depend on.)
if [ "$FILTERED" = no ]; then
    printf '%s\n' "Running $SELECTED_COUNT fpmng-owned tests" >&2
else
    printf '%s\n' "Running $SELECTED_COUNT of $TEST_COUNT fpmng-owned tests (filter: $FILTER_DESC)" >&2
fi
printf '%s\n' "Binary: $FPM_BIN" >&2

# shellcheck disable=SC2046
TEST_FILES=$(tr '\n' ' ' < "$SELECTED")

set +e
# $SHOW_SLOW_ARGS and $TEST_FILES are both meant to word-split into separate
# argv entries, which is what SC2086 would otherwise stop. (The reason is on
# its own line: shellcheck does not parse a "disable=CODE -- reason" directive,
# which is where this repository's six existing SC1072/SC1073 errors come from.)
# shellcheck disable=SC2086
(
    cd "$PHPSRC" || exit 1
    TEST_PHP_EXECUTABLE="$HARNESS_CLI" \
    TEST_PHP_FPM_EXECUTABLE="$HARNESS_FPM" \
    "$CLI_BIN" -n run-tests.php \
        -q -n --offline --no-progress --no-color \
        --set-timeout "$TIMEOUT" \
        -p "$HARNESS_CLI" \
        -W "$STATUS_RAW" \
        -w "$FAILED" \
        -s "$OUTPUT_LOG" \
        $SHOW_SLOW_ARGS \
        $TEST_FILES
) > "$RUN_LOG" 2>&1
RUN_STATUS=$?
set -e
[ -f "$STATUS_RAW" ] || : > "$STATUS_RAW"
[ -f "$FAILED" ] || : > "$FAILED"
[ -f "$OUTPUT_LOG" ] || : > "$OUTPUT_LOG"

{
    printf 'test\tcategory\traw_status\n'
    awk -F '\t' -v status_file="$STATUS_RAW" -v source_root="$PHPSRC" '
        function normalize(path, prefix) {
            sub(/^\.\//, "", path)
            prefix = source_root "/"
            if (index(path, prefix) == 1) {
                path = substr(path, length(prefix) + 1)
            }
            return path
        }
        FILENAME == status_file {
            if (NF >= 2) {
                status[normalize($2)] = $1
            }
            next
        }
        {
            key = normalize($0)
            raw = status[key]
            if (raw == "PASSED") {
                category = "PASS"
            } else if (raw == "SKIPPED") {
                category = "SKIP"
            } else if (raw == "WARNED") {
                category = "WARN"
            } else if (raw == "") {
                raw = "NOT_MEASURED"
                category = "NOT MEASURED"
            } else {
                category = "FAIL/ERROR"
            }
            printf "%s\t%s\t%s\n", $0, category, raw
        }
    ' "$STATUS_RAW" "$SELECTED"
} > "$RESULTS"

NOT_MEASURED_COUNT=$(awk -F '\t' 'NR > 1 && $2 == "NOT MEASURED" {count++} END {print count + 0}' "$RESULTS")
if [ "$NOT_MEASURED_COUNT" -gt 0 ]; then
    MEASUREMENT_STATUS=PARTIAL
    BLOCKER='run-tests.php did not emit a status for every selected test; see run.log'
else
    MEASUREMENT_STATUS=MEASURED
    BLOCKER=none
fi
PASS_COUNT=$(awk -F '\t' 'NR > 1 && $2 == "PASS" {count++} END {print count + 0}' "$RESULTS")

# See the TEST_FPM_MIN_PASS block above for why this exists and why it is a
# fraction. Not enforced on a filtered run unless a count was given explicitly:
# a filter may legitimately select nothing but tests this binary skips, which is
# a useful thing to be able to do and not a broken run.
LOW_PASS=
MIN_PASS_CHECK=skipped
if [ "$MIN_PASS" = 0 ]; then
    MIN_PASS_CHECK=off
elif [ -n "$MIN_PASS" ]; then
    MIN_PASS_CHECK="$PASS_COUNT >= $MIN_PASS"
    if [ "$PASS_COUNT" -lt "$MIN_PASS" ]; then
        LOW_PASS="only $PASS_COUNT of $SELECTED_COUNT selected tests passed; TEST_FPM_MIN_PASS demands $MIN_PASS"
    fi
elif [ "$FILTERED" = no ]; then
    MIN_PASS_CHECK="$PASS_COUNT of $SELECTED_COUNT, floor is half"
    if [ "$((PASS_COUNT * 2))" -lt "$SELECTED_COUNT" ]; then
        LOW_PASS="only $PASS_COUNT of $SELECTED_COUNT tests passed, fewer than half; a run that skips everything and exits 0 is the trap this checks for (as root php-fpm refuses to start). Read $RESULTS for the skip reasons, or set TEST_FPM_MIN_PASS if this binary really cannot pass more"
    fi
fi

{
    printf 'measurement_status=%s\n' "$MEASUREMENT_STATUS"
    printf 'run_exit_status=%s\n' "$RUN_STATUS"
    printf 'blocker=%s\n' "$BLOCKER"
    printf 'min_pass_check=%s\n' "$MIN_PASS_CHECK"
    write_counts
} > "$SUMMARY"

# run-tests.php prints its SLOW TEST SUMMARY only into the summary it writes
# with -s, already sorted longest-first, as "(12.345 s) <name> [<file>]".
# Reshape it into a two-column TSV (seconds, test file) so the numbers can be
# diffed between runs, and echo the table to stderr so it is readable in the CI
# log without downloading the artifact.
#
# Two things about that block that a first version of this got wrong, both
# visible in the artifact from run 35274092140:
#
#   1. run-tests.php writes the whole summary TWICE into the -s file, so a naive
#      parse produced 70 rows for 35 tests and a total of 501.8 s for a step
#      that took 266.4. Only the first block is read.
#   2. An entry is NOT always one line. The name comes from the test's --TEST--
#      section verbatim, and several of ours are long enough to have been
#      written across two lines -- which puts the " [<file>]" on the
#      continuation, so those rows were labelled with a sentence fragment
#      instead of a path. Lines are accumulated until the bracket closes.
: > "$SLOW"
if [ "$SHOW_SLOW_MS" -gt 0 ] && [ -f "$OUTPUT_LOG" ]; then
    awk '
        function flush() {
            if (rec == "") return
            secs = substr(rec, 2, index(rec, " s) ") - 2)
            file = substr(rec, index(rec, " s) ") + 4)
            # The name ends in " [sapi/fpmng/tests/x.phpt]"; keep just the path.
            # No bracket means the entry was cut off -- keep the name, so the row
            # is still readable, rather than dropping the measurement.
            if (match(file, /\[[^]]*\]$/)) {
                file = substr(file, RSTART + 1, RLENGTH - 2)
            }
            printf "%s\t%s\n", secs, file
            rec = ""
        }
        /^SLOW TEST SUMMARY$/ { if (seen) { done = 1 } ; seen = 1; inblock = !done; next }
        done { next }
        inblock && /^====/ { flush(); inblock = 0; next }
        inblock && /^----/ { next }
        inblock && /^\([0-9.]+ s\) / { flush(); rec = $0; next }
        inblock && rec != "" && NF > 0 { rec = rec " " $0; next }
        inblock && NF == 0 { flush(); next }
        END { flush() }
    ' "$OUTPUT_LOG" > "$SLOW" 2>/dev/null || : > "$SLOW"
fi

if [ -s "$SLOW" ]; then
    printf '%s\n' "Tests slower than ${SHOW_SLOW_MS}ms, longest first:" >&2
    awk -F '\t' '{ total += $1; printf "  %8.1fs  %s\n", $1, $2 }
                  END { printf "  %8.1fs  = these %d tests together\n", total, NR }' "$SLOW" >&2
fi

cat "$SUMMARY" >&2
printf '%s\n' "Raw run log: $RUN_LOG" >&2
printf '%s\n' "Per-test results: $RESULTS" >&2
if [ -s "$SLOW" ]; then
    printf '%s\n' "Per-test durations: $SLOW" >&2
fi
# After the summary and the slow table, not instead of them: the counts and the
# durations are exactly what someone debugging this failure needs to see.
if [ -n "$LOW_PASS" ]; then
    printf '%s\n' "run-fpmng-phpt.sh: $LOW_PASS" >&2
    [ "$RUN_STATUS" -ne 0 ] || RUN_STATUS=1
fi

exit "$RUN_STATUS"
