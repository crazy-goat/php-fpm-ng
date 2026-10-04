#!/bin/sh
# Prepares a php-src tree for the from-source FIBER build: main's overlay
# (build/prepare.sh), then the fiber source split, then the two patches.
#
#   async/prepare.sh /path/to/php-src
#
# Branch async only. build/prepare.sh is main's file and knows three source
# groups (base, TLS, ACME); it would put the fiber/coop translation units
# into the always-built base list. This script takes them out again and
# substitutes the group sapi/fpmng/config.m4 declares on this branch
# (@FPMNG_FIBER_SOURCES@ under --enable-fpmng-fiber). Matching is by NAME PREFIX,
# never by enumerating files, so a new fpm_pool_coop_whatever.c lands in the
# fiber group instead of silently in the default binary:
#   fiber group  fpm/fpm_pool_(fiber|coop)*.c   (coop is used only by fiber)
set -eu

PHPSRC="${1:?usage: async/prepare.sh <php-src-dir>}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"

"$REPO/build/prepare.sh" "$PHPSRC"

CM4="$PHPSRC/sapi/fpmng/config.m4"
FIBER_RE='^fpm/fpm_pool_(fiber|coop)[A-Za-z0-9_]*\.c$'

ALL=$(cd "$REPO/sapi/fpmng" && find fpm -name '*.c' | LC_ALL=C sort)
FIBER_SOURCES=$(echo "$ALL" | grep -E "$FIBER_RE" || true)
[ -n "$FIBER_SOURCES" ] || { echo "async/prepare.sh: no fiber/coop files found" >&2; exit 1; }
if ! grep -q '@FPMNG_FIBER_SOURCES@' "$CM4"; then
  echo "async/prepare.sh: $CM4 has no fiber placeholder (build/prepare.sh already substituted or config.m4 changed)" >&2
  exit 1
fi

list() { echo "$1" | sed 's/$/ \\/' | sed 's/^/    /'; }
# Through the environment, not awk -v: -v would interpret the backslashes.
FIBER_LIST=$(list "$FIBER_SOURCES")
export FIBER_LIST

awk '
  /^[[:space:]]+fpm\/fpm_pool_(fiber|coop)[A-Za-z0-9_]*\.c \\$/ { next }
  {
    # the base list from build/prepare.sh is written on one line, so drop the tokens too.
    gsub(/fpm\/fpm_pool_(fiber|coop)[A-Za-z0-9_]*\.c[ ]*/, "");
    gsub(/@FPMNG_FIBER_SOURCES@/, "\n" ENVIRON["FIBER_LIST"] "\n  ");
    print
  }' "$CM4" > "$CM4.tmp"
mv "$CM4.tmp" "$CM4"

"$REPO/async/apply-patches.sh" "$PHPSRC"

echo "async/prepare.sh: fiber group $(echo "$FIBER_SOURCES" | wc -l | tr -d ' ') files"
echo "  an existing build directory has a frozen object list: run ./buildconf --force and reconfigure"
