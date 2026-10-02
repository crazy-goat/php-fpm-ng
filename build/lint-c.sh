#!/bin/sh
# Static-analysis pass over *our* C sources only (task 013).
#
# File list comes exclusively from this repository. Never walk the assembled
# tree under a libphp build directory: it mixes our files with the vendored
# upstream copies (third_party/php-src), and linting it would report on code we
# do not own.
#
# Usage:
#   ./build/lint-c.sh [libphp-build-outdir]
#
# The directory is the one build/libphp-build.sh wrote (issue #424: there is no
# configured php-src tree to ask any more). Its commands.log holds the exact
# compile line of every translation unit, and the -I/-D flags are taken from
# there, so php.h and the supplied feature macros resolve the way the real
# build resolves them. Build it with the same FPMNG_TLS/FPMNG_ACME/
# FPMNG_DEBUG_CLOCK toggles as the binary under test, or the files behind
# those toggles are linted with their macros undefined. Without a directory
# clang-tidy still runs; expect missing-header noise -- useful only as a smoke
# check of the config.
#
# Exit status: clang-tidy's. With WarningsAsErrors: '*' in .clang-tidy
# (issue #414) any finding in our TUs/headers fails this script; the CI step
# propagates its status via rc=$?; exit $rc.
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_OUT="${1:-}"

if ! command -v clang-tidy >/dev/null 2>&1; then
	echo "lint-c: clang-tidy not found in PATH" >&2
	exit 127
fi

# Our translation units only. Headers are covered via HeaderFilterRegex when
# a .c includes them; we do not pass .h files as TUs on their own.
FILES=$(find "$REPO/sapi/fpmng" "$REPO/ext/fpmng_metrics" \
	-type f -name '*.c' ! -name '*.notbuilt' | sort)
# Guard: zero files means the paths moved and the boundary broke silently.
[ -n "$FILES" ] || { echo "lint-c: no .c files under sapi/fpmng or ext/fpmng_metrics" >&2; exit 1; }

EXTRA_ARGS="-std=gnu11 -Wno-unknown-warning-option"
# Always prefer our own headers over the assembled tree's copies.
EXTRA_ARGS="$EXTRA_ARGS -I$REPO/sapi/fpmng -I$REPO/sapi/fpmng/fpm"
EXTRA_ARGS="$EXTRA_ARGS -I$REPO/ext/fpmng_metrics"

if [ -n "$BUILD_OUT" ]; then
	LOG="$BUILD_OUT/commands.log"
	[ -f "$LOG" ] || { echo "lint-c: no commands.log in $BUILD_OUT -- not a build/libphp-build.sh output directory" >&2; exit 1; }
	# Every compile line carries the same -D/-I set; the first one is enough.
	# A log with no compile line would lint with none of them, which is the
	# silent no-op this guard exists to refuse.
	CMD=$(grep -m1 ' -c ' "$LOG" || true)
	[ -n "$CMD" ] || { echo "lint-c: $LOG holds no compile command" >&2; exit 1; }
	FLAGS=""
	for tok in $CMD; do
		case "$tok" in
		-I*|-D*) FLAGS="$FLAGS $tok" ;;
		esac
	done
	EXTRA_ARGS="$EXTRA_ARGS $FLAGS"
	echo "lint-c: compile flags from $LOG"
fi

# EXTRA_ARGS and FILES are intentionally word-split; paths have no spaces.
# shellcheck disable=SC2086
set -- $FILES
echo "lint-c: $# translation units"
# Header filter must be the absolute repo paths: a bare 'sapi/fpmng/' also
# matches the assembled tree's sapi/fpmng/ (upstream copies we do not own).
# shellcheck disable=SC2086
clang-tidy \
	--config-file="$REPO/.clang-tidy" \
	--header-filter="^$REPO/(sapi/fpmng|ext/fpmng_metrics)/" \
	--quiet \
	"$@" -- $EXTRA_ARGS
