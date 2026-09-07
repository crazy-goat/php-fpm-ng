#!/usr/bin/env bash
set -Eeuo pipefail

# One documented command for task 027 (acceptance criterion 1): run every
# framework probe (Symfony, Laravel, Slim 4) against one built php-fpm-ng
# binary, with each probe provisioning its own services via Docker Compose
# by default, so the whole suite works on a clean machine with nothing
# pre-provisioned but Docker, Composer/PHP, and the binary itself.
#
# Usage:
#   FPMNG_BIN=/path/to/php-fpm-ng [PHP_BIN=/path/to/php] tests/frameworks/run-all.sh
#
# FPMNG_BIN is required. PHP_BIN (or PHP) is optional and defaults to "php"
# on PATH; it is forwarded to each runner under the env var name that runner
# itself reads (Symfony reads PHP_BIN, Laravel and Slim 4 read PHP).
#
# The three runners run sequentially, not in parallel: each defaults to its
# own fixed FastCGI/HTTP ports and, in SERVICE_MODE=docker, its own default
# MySQL/Redis host ports, and a busy machine could make two runners racing
# for the "next free port" collide with each other. Sequential keeps this
# harness simple; parallelizing them is future work, not scope for task 027.
#
# Executor matrix: fiber-only. Every scenario here exercises
# pool.executor = fiber, per the decision recorded in
# tasks/done/027-framework-integration-test-harness.md. Default-executor
# coverage is explicitly out of scope for this harness.
#
# Exit status is non-zero unless every framework's runner reports PASS; a
# framework that could not run (NOT MEASURED) is not a pass. Laravel's own
# runner exits non-zero whenever its negative controls correctly demonstrate
# the expected failure (by design, see laravel/README.md); this script
# reinterprets that from Laravel's own SUMMARY line (see classify_laravel
# below) rather than from its raw exit status, so a fully healthy Laravel
# run still reports PASS here.

SUITE_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
RUN_ID=${FPMNG_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)-$$}
RUN_ROOT=${FPMNG_RUN_ROOT:-$SUITE_DIR/.runs/$RUN_ID}
mkdir -p "$RUN_ROOT"

FPMNG_BIN=${FPMNG_BIN:-}
if [[ -z $FPMNG_BIN ]]; then
    echo "FPMNG_BIN is required: point it at a built php-fpm-ng binary" >&2
    exit 2
fi
if [[ ! -x $FPMNG_BIN ]]; then
    echo "FPMNG_BIN is not an executable file: $FPMNG_BIN" >&2
    exit 2
fi
FPMNG_BIN=$(cd -- "$(dirname -- "$FPMNG_BIN")" && pwd)/$(basename -- "$FPMNG_BIN")

# Resolve one PHP CLI binary and hand it to each runner under the env var
# name that runner reads (they differ; see the usage comment above).
RESOLVED_PHP=${PHP_BIN:-${PHP:-php}}

FRAMEWORK_NAMES=(symfony laravel slim4)
FRAMEWORK_STATUS=()
FRAMEWORK_DETAIL=()

run_symfony() {
    local log=$1
    FPMNG_BIN="$FPMNG_BIN" \
    PHP_BIN="$RESOLVED_PHP" \
    SERVICE_MODE=${SERVICE_MODE:-docker} \
    FPMNG_RUN_ID="symfony-$RUN_ID" \
        "$SUITE_DIR/symfony/run.sh" >"$log" 2>&1
}

run_laravel() {
    local log=$1
    # Default to the predis client so the one documented command completes
    # on a machine with nothing but Docker/Composer/PHP/the binary: Laravel's
    # own default (REDIS_CLIENT=phpredis) requires a REDIS_EXTENSION pointing
    # at a compiled redis.so, which is not part of a clean-machine baseline.
    # predis/predis is already a locked Composer dependency. A caller who
    # sets REDIS_CLIENT explicitly, or sets REDIS_EXTENSION to get the
    # phpredis measurement, is respected as-is.
    local laravel_redis_client=${REDIS_CLIENT:-}
    if [[ -z $laravel_redis_client && -z ${REDIS_EXTENSION:-} ]]; then
        laravel_redis_client=predis
    fi

    REDIS_CLIENT="$laravel_redis_client" \
    FPMNG="$FPMNG_BIN" \
    PHP="$RESOLVED_PHP" \
    SERVICE_MODE=${SERVICE_MODE:-docker} \
    RUN_DIR="$RUN_ROOT/laravel-run" \
        "$SUITE_DIR/laravel/bin/run.sh" >"$log" 2>&1
}

run_slim4() {
    local log=$1
    FPMNG="$FPMNG_BIN" \
    PHP="$RESOLVED_PHP" \
    SERVICE_MODE=${SERVICE_MODE:-docker} \
    RUN_DIR="$RUN_ROOT/slim4-run" \
        "$SUITE_DIR/slim4/bin/run.sh" >"$log" 2>&1
}

# classify_exit STATUS -> the vocabulary the individual runners already use:
# PASS (status 0), ERROR (assertions failed, status 1), or NOT MEASURED
# (setup could not run, any other status, conventionally 2). A framework
# that could not run must never be counted as a pass.
classify_exit() {
    case $1 in
        0) echo PASS ;;
        1) echo ERROR ;;
        *) echo "NOT MEASURED" ;;
    esac
}

# summary_line LOG PATTERN -> the last matching line, which is each runner's
# own aggregate summary (reusing their vocabulary instead of re-deriving one
# from individual scenario lines).
summary_line() {
    local log=$1
    local pattern=$2
    grep -E "$pattern" "$log" | tail -n 1
}

# classify_laravel LINE -> Laravel's bin/run.sh exits non-zero whenever any
# negative-control scenario reports ERROR, which is the *expected, correct*
# outcome of a working negative control (see findings.md and
# laravel/README.md: a healthy run is configured_error=0, negative_error>0).
# So its raw exit status cannot tell "the harness is healthy" apart from
# "an unexpected assertion failed" — parse its own SUMMARY line instead:
# PASS when the configured suite has no errors (negative-control errors are
# the correct, designed outcome, not a harness failure); ERROR when a
# configured scenario failed unexpectedly; NOT MEASURED when the line is
# absent (setup did not get far enough to print it at all).
classify_laravel() {
    local line=$1
    local configured_error

    [[ -n $line ]] || { echo "NOT MEASURED"; return; }
    if [[ $line =~ configured_error=([0-9]+) ]]; then
        configured_error=${BASH_REMATCH[1]}
    else
        echo "NOT MEASURED"
        return
    fi
    if (( configured_error > 0 )); then
        echo ERROR
    else
        echo PASS
    fi
}

run_one() {
    local name=$1
    local log=$RUN_ROOT/$name.log
    local status
    local detail

    echo "==> running $name ($log)"
    if "run_$name" "$log"; then
        status=0
    else
        status=$?
    fi

    local verdict
    case $name in
        symfony)
            detail=$(summary_line "$log" '^Summary: PASS=')
            verdict=$(classify_exit "$status")
            ;;
        laravel)
            detail=$(summary_line "$log" '^SUMMARY configured_pass=')
            # Not classify_exit: see classify_laravel's comment above for why
            # Laravel's raw exit status is not "PASS means the harness is
            # healthy" here.
            verdict=$(classify_laravel "$detail")
            ;;
        slim4)
            detail=$(summary_line "$log" '^SUMMARY pass=')
            verdict=$(classify_exit "$status")
            ;;
    esac
    [[ -n $detail ]] || detail="(no summary line found; see $log)"

    FRAMEWORK_STATUS+=("$verdict")
    FRAMEWORK_DETAIL+=("$detail")
}

main() {
    local name
    local overall=0
    local index

    for name in "${FRAMEWORK_NAMES[@]}"; do
        run_one "$name"
    done

    echo
    echo "=== tests/frameworks/run-all.sh summary (run $RUN_ID) ==="
    for index in "${!FRAMEWORK_NAMES[@]}"; do
        printf '%-8s %-14s %s\n' "${FRAMEWORK_NAMES[$index]}" \
            "${FRAMEWORK_STATUS[$index]}" "${FRAMEWORK_DETAIL[$index]}"
        # A framework that could not run (NOT MEASURED) is a failure of this
        # command's job, exactly as much as ERROR is: it must never count
        # as a pass (task 027, acceptance criterion 4).
        [[ ${FRAMEWORK_STATUS[$index]} == PASS ]] || overall=1
    done
    echo
    echo "Logs: $RUN_ROOT"

    return "$overall"
}

main "$@"
