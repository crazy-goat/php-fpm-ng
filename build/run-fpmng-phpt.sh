#!/bin/sh
# Run php-fpm-ng's own fpmng-*.phpt regression tests (task 003).
#
# Usage:
#   TEST_PHP_EXECUTABLE=/path/to/php \
#   TEST_PHP_FPM_EXECUTABLE=/path/to/php-fpm-ng \
#   build/run-fpmng-phpt.sh /path/to/prepared-php-src /path/to/results
#
# The prepared tree must contain sapi/fpmng/tests/ with tester.inc from
# build/prepare.sh. Only files matching fpmng-*.phpt are executed — upstream's
# copied suite is intentionally excluded.
set -eu

usage() {
    cat >&2 <<'EOF'
Usage: TEST_PHP_EXECUTABLE=/path/to/php \
       TEST_PHP_FPM_EXECUTABLE=/path/to/php-fpm-ng \
       build/run-fpmng-phpt.sh /path/to/prepared-php-src /path/to/results

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

[ "$#" -eq 2 ] || usage

PHPSRC_INPUT=$1
RESULTS_INPUT=$2
TEST_DIR=sapi/fpmng/tests

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
STATUS_RAW=$RESULTS_DIR/statuses.raw.tsv
RESULTS=$RESULTS_DIR/results.tsv
FAILED=$RESULTS_DIR/failed.raw.txt
RUN_LOG=$RESULTS_DIR/run.log
OUTPUT_LOG=$RESULTS_DIR/test-output.log
METADATA=$RESULTS_DIR/metadata.txt
SUMMARY=$RESULTS_DIR/summary.txt

rm -f "$DISCOVERED" "$STATUS_RAW" "$RESULTS" "$FAILED" "$RUN_LOG" "$OUTPUT_LOG" "$METADATA" "$SUMMARY"

if ! (
    cd "$PHPSRC"
    find "$TEST_DIR" -maxdepth 1 -type f -name 'fpmng-*.phpt' -print | LC_ALL=C sort
) > "$DISCOVERED"; then
    fail "cannot enumerate fpmng-owned .phpt tests"
fi
TEST_COUNT=$(awk 'END {print NR + 0}' "$DISCOVERED")
[ "$TEST_COUNT" -gt 0 ] || fail "no fpmng-*.phpt tests were discovered"

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
        done < "$DISCOVERED"
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
MARKERS=$(printf '%s\n' "$FPM_STRINGS" | grep -E 'php-fpm-ng|pool\.type|fpmng_|fastcgi-ng' | LC_ALL=C sort -u || true)
printf '%s\n' "$MARKERS" | grep -q 'fpmng_' || preflight_fail "strings has no fpmng_ marker: $FPM_BIN"
printf '%s\n' "$MARKERS" | grep -Eq 'php-fpm-ng|pool\.type|fastcgi-ng' || preflight_fail "strings has no pool.type/php-fpm-ng marker: $FPM_BIN"

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

printf '%s\n' "Running $TEST_COUNT fpmng-owned tests" >&2
printf '%s\n' "Binary: $FPM_BIN" >&2

# shellcheck disable=SC2046
TEST_FILES=$(tr '\n' ' ' < "$DISCOVERED")

set +e
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
    ' "$STATUS_RAW" "$DISCOVERED"
} > "$RESULTS"

NOT_MEASURED_COUNT=$(awk -F '\t' 'NR > 1 && $2 == "NOT MEASURED" {count++} END {print count + 0}' "$RESULTS")
if [ "$NOT_MEASURED_COUNT" -gt 0 ]; then
    MEASUREMENT_STATUS=PARTIAL
    BLOCKER='run-tests.php did not emit a status for every discovered test; see run.log'
else
    MEASUREMENT_STATUS=MEASURED
    BLOCKER=none
fi
{
    printf 'measurement_status=%s\n' "$MEASUREMENT_STATUS"
    printf 'run_exit_status=%s\n' "$RUN_STATUS"
    printf 'blocker=%s\n' "$BLOCKER"
    write_counts
} > "$SUMMARY"

cat "$SUMMARY" >&2
printf '%s\n' "Raw run log: $RUN_LOG" >&2
printf '%s\n' "Per-test results: $RESULTS" >&2
exit "$RUN_STATUS"
