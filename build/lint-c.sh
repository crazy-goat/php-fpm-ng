#!/bin/sh
# Static-analysis pass over *our* C sources only (task 013).
#
# File list comes exclusively from this repository. Never walk a prepared
# php-src tree: after build/prepare.sh that tree mixes untouched upstream
# copies with our overlays, and linting it would report on code we do not own.
#
# Usage:
#   ./build/lint-c.sh [prepared-php-src]
#
# If a prepared php-src path is given (configure already run), its -I paths
# are passed through so php.h and friends resolve. Without it, clang-tidy still
# runs; expect missing-header noise — useful only as a smoke check of the
# config file itself.
#
# Exit status: clang-tidy's. CI treats this job as non-blocking for now.
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PHP_SRC="${1:-}"

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

EXTRA_ARGS="-std=gnu11"
EXTRA_ARGS="$EXTRA_ARGS -I$REPO/sapi/fpmng -I$REPO/sapi/fpmng/fpm"
EXTRA_ARGS="$EXTRA_ARGS -I$REPO/ext/fpmng_metrics"

if [ -n "$PHP_SRC" ]; then
	[ -d "$PHP_SRC" ] || { echo "lint-c: not a directory: $PHP_SRC" >&2; exit 1; }
	# After ./configure these exist; without them analyzer checks are useless.
	for d in . main Zend TSRM sapi/fpmng sapi/fpmng/fpm ext/standard; do
		[ -d "$PHP_SRC/$d" ] && EXTRA_ARGS="$EXTRA_ARGS -I$PHP_SRC/$d"
	done
	# php_config.h lives in the build tree root after configure (in-tree or out).
	if [ -f "$PHP_SRC/main/php_config.h" ] || [ -f "$PHP_SRC/php_config.h" ]; then
		:
	else
		echo "lint-c: warning: no php_config.h under $PHP_SRC — run configure first" >&2
	fi
	# HAVE_* that our files expect when built as the fpmng SAPI.
	EXTRA_ARGS="$EXTRA_ARGS -DHAVE_CONFIG_H -DFPMNG"
fi

# EXTRA_ARGS and FILES are intentionally word-split; paths have no spaces.
# shellcheck disable=SC2086
set -- $FILES
echo "lint-c: $# translation units"
# --quiet keeps the report to findings; config file is the check contract.
# No -p / compile_commands: the file list is ours, compiler args follow --.
# shellcheck disable=SC2086
clang-tidy --config-file="$REPO/.clang-tidy" --quiet "$@" -- $EXTRA_ARGS
