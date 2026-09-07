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
# If a prepared php-src path is given (configure already run), include paths
# are taken from that tree's Makefile so php.h / fpm_config.h / config.h
# resolve the same way a real build would. Without it, clang-tidy still runs;
# expect missing-header noise — useful only as a smoke check of the config.
#
# Exit status: clang-tidy's. CI treats this job as non-blocking for now.
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PHP_SRC="${1:-}"
STUB=""

cleanup() {
	[ -n "$STUB" ] && rm -rf "$STUB"
}
trap cleanup EXIT INT TERM

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
# Always prefer our overlay headers over anything prepare.sh copied.
EXTRA_ARGS="$EXTRA_ARGS -I$REPO/sapi/fpmng -I$REPO/sapi/fpmng/fpm"
EXTRA_ARGS="$EXTRA_ARGS -I$REPO/ext/fpmng_metrics"

if [ -n "$PHP_SRC" ]; then
	[ -d "$PHP_SRC" ] || { echo "lint-c: not a directory: $PHP_SRC" >&2; exit 1; }
	PHP_SRC="$(cd "$PHP_SRC" && pwd)"

	if [ ! -f "$PHP_SRC/main/php_config.h" ] && [ ! -f "$PHP_SRC/php_config.h" ]; then
		echo "lint-c: warning: no php_config.h under $PHP_SRC — run configure first" >&2
	fi

	# ext/* and some Zend headers do `#include "config.h"` under HAVE_CONFIG_H.
	# The real build generates that next to each extension; for tidy we point at
	# a stub that forwards to php_config.h (same content the in-tree build uses).
	STUB=$(mktemp -d "${TMPDIR:-/tmp}/fpmng-lint.XXXXXX")
	printf '%s\n' '#include "php_config.h"' > "$STUB/config.h"
	EXTRA_ARGS="$EXTRA_ARGS -I$STUB -DHAVE_CONFIG_H"

	# Prefer the include line the configured Makefile already computed — it
	# knows about Zend/TSRM/main/sapi paths and any --with-* -I flags.
	if [ -f "$PHP_SRC/Makefile" ]; then
		# EXTRA_INCLUDES is space-separated -I... tokens on one assign line.
		MAKE_INCLUDES=$(sed -n 's/^EXTRA_INCLUDES *= *//p' "$PHP_SRC/Makefile" | head -1)
		if [ -n "$MAKE_INCLUDES" ]; then
			EXTRA_ARGS="$EXTRA_ARGS $MAKE_INCLUDES"
		fi
	fi

	# Fall back / supplement with the usual php-src layout (in-tree build).
	for d in . main Zend TSRM sapi/fpmng sapi/fpmng/fpm ext/standard ext/date/lib; do
		[ -d "$PHP_SRC/$d" ] && EXTRA_ARGS="$EXTRA_ARGS -I$PHP_SRC/$d"
	done
fi

# EXTRA_ARGS and FILES are intentionally word-split; paths have no spaces.
# shellcheck disable=SC2086
set -- $FILES
echo "lint-c: $# translation units"
# --quiet keeps the report to findings; config file is the check contract.
# No -p / compile_commands: the file list is ours, compiler args follow --.
# shellcheck disable=SC2086
clang-tidy --config-file="$REPO/.clang-tidy" --quiet "$@" -- $EXTRA_ARGS
