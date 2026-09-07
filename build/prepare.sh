#!/bin/sh
# Assembles sapi/fpmng/ into a php-src tree: first the stable FPM sources from
# upstream, then our files on top. The repo keeps only what is ours.
#
#   build/prepare.sh /path/to/php-src
#
# Upstream is not modified — only the new sapi/fpmng/ directory is created.
set -e
PHPSRC="${1:?provide a path to php-src}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"

[ -d "$PHPSRC/sapi/fpm" ] || { echo "this does not look like php-src: $PHPSRC" >&2; exit 1; }

# MUST run BEFORE the directory is rebuilt: below, sapi/fpmng is deleted and
# recreated from the upstream sapi/fpm, which has no PHP_FPMNG_*_FILES blocks.
# Used to detect whether a .c file appeared or disappeared — see the warning
# at the end. The list is split into three blocks (base, fiber, async) — see
# below — but for change detection we take them together, because the whole
# set matters regardless of which block a file landed in.
OLD_SOURCES=""
if [ -f "$PHPSRC/sapi/fpmng/config.m4" ]; then
  # Only the PHP_FPMNG_*_FILES="..." blocks — config.m4 also mentions other
  # files (fpm_systemd.c, fpm_trace.c, www.c under conditions) that do not
  # belong to any of these lists.
  OLD_SOURCES=$( { \
      sed -n '/PHP_FPMNG_FILES="/,/^[[:space:]]*"[[:space:]]*$/p' \
        "$PHPSRC/sapi/fpmng/config.m4"; \
      sed -n '/PHP_FPMNG_FIBER_FILES="/,/^[[:space:]]*"[[:space:]]*$/p' \
        "$PHPSRC/sapi/fpmng/config.m4"; \
      sed -n '/PHP_FPMNG_ASYNC_FILES="/,/^[[:space:]]*"[[:space:]]*$/p' \
        "$PHPSRC/sapi/fpmng/config.m4"; \
    } | grep -oE 'fpm/[A-Za-z0-9_/]+\.c' | sort -u)
fi

rm -rf "$PHPSRC/sapi/fpmng"
cp -r "$PHPSRC/sapi/fpm" "$PHPSRC/sapi/fpmng"

# Keep the upstream tests in the copied SAPI. The runner supplies the binary path
# through TEST_PHP_FPM_EXECUTABLE, so the tests do not need to be forked here.

# Our files override upstream.
cp -r "$REPO/sapi/fpmng/." "$PHPSRC/sapi/fpmng/"

# The fpmng_metrics extension (NOTES 3k): ext/ is discovered by the same glob
# as sapi/, so also zero patches against upstream. This repo's directory:
[ -d "$REPO/ext/fpmng_metrics" ] && {
  rm -rf "$PHPSRC/ext/fpmng_metrics"
  cp -r "$REPO/ext/fpmng_metrics" "$PHPSRC/ext/fpmng_metrics"
}

# The source list comes from the config.m4 of THIS php-src, not our copy —
# otherwise it drifts on every upstream change (e.g. removal of events/devpoll.c).
SOURCES=$(sed -n '/PHP_FPM_FILES="/,/^[[:space:]]*"[[:space:]]*$/p' "$PHPSRC/sapi/fpm/config.m4" \
  | grep -oE 'fpm/[A-Za-z0-9_/]+\.c' \
  | sort -u)
[ -n "$SOURCES" ] || { echo "failed to read the source list from sapi/fpm/config.m4" >&2; exit 1; }

# Our own .c files join the list when upstream does not have them.
for f in $(cd "$REPO/sapi/fpmng" && find fpm -name '*.c' | sort); do
  echo "$SOURCES" | grep -qx "$f" || SOURCES="$SOURCES
$f"
done

# Split into three groups: fiber (fiber + the whole coop layer, used only by
# fiber) and async (fpm_pool_async.c) go under --enable-fpmng-fiber /
# --enable-fpmng-async (both default "no"); the rest is always built.
# The split was verified against symbol references — coop.* is not used
# outside fiber, async does not reference coop.
# Matching by NAME PREFIX, not by enumerating files. Enumeration would undo
# the whole point of this script: the source list must come from 'find' so a
# new file needs no edit. With enumeration, a new fpm_pool_coop_whatever.c
# would NOT match the pattern, would silently land in the base list, and end
# up in the default binary — exactly what these flags are meant to prevent.
FIBER_PATTERN='^fpm/fpm_pool_(fiber|coop)[A-Za-z0-9_]*\.c$'
ASYNC_PATTERN='^fpm/fpm_pool_async\.c$'

BASE_SOURCES=$(echo "$SOURCES" | grep -Ev "$FIBER_PATTERN" | grep -Ev "$ASYNC_PATTERN")
FIBER_SOURCES=$(echo "$SOURCES" | grep -E "$FIBER_PATTERN")
ASYNC_SOURCES=$(echo "$SOURCES" | grep -E "$ASYNC_PATTERN")

[ -n "$FIBER_SOURCES" ] || { echo "no fiber/coop files found in the source list" >&2; exit 1; }
[ -n "$ASYNC_SOURCES" ] || { echo "fpm_pool_async.c not found in the source list" >&2; exit 1; }

BASE_LIST=$(echo "$BASE_SOURCES" | sed 's/$/ \\/' | sed 's/^/    /')
FIBER_LIST=$(echo "$FIBER_SOURCES" | sed 's/$/ \\/' | sed 's/^/    /')
ASYNC_LIST=$(echo "$ASYNC_SOURCES" | sed 's/$/ \\/' | sed 's/^/    /')

awk -v base="$BASE_LIST" -v fiber="$FIBER_LIST" -v async="$ASYNC_LIST" '{
    gsub(/@FPMNG_SOURCES@/, "\n" base "\n  ");
    gsub(/@FPMNG_FIBER_SOURCES@/, "\n" fiber "\n  ");
    gsub(/@FPMNG_ASYNC_SOURCES@/, "\n" async "\n  ");
    print
  }' "$PHPSRC/sapi/fpmng/config.m4" > "$PHPSRC/sapi/fpmng/config.m4.tmp"
mv "$PHPSRC/sapi/fpmng/config.m4.tmp" "$PHPSRC/sapi/fpmng/config.m4"

# Did the source list change since the previous run? If so, the existing build
# directory has a FROZEN object list in the Makefile and will not see the new
# file. This shows up as a link error AFTER everything recompiles — or, when
# the new file exports no symbols used by others, not at all: the build passes
# and the binary silently leaves out the new pool type. Hence a loud warning
# at the end, so it does not scroll off the screen.
# Note: this script is /bin/sh, so no <(...) — the comparison goes through
# temporary files.
SOURCES_CHANGED=""
NEW_SORTED=$(echo "$SOURCES" | sort -u)
if [ -n "$OLD_SOURCES" ] && [ "$OLD_SOURCES" != "$NEW_SORTED" ]; then
  _old=$(mktemp) && _new=$(mktemp)
  printf '%s\n' "$OLD_SOURCES" > "$_old"
  printf '%s\n' "$NEW_SORTED" > "$_new"
  SOURCES_CHANGED=$(diff "$_old" "$_new" | grep '^[<>]' || true)
  rm -f "$_old" "$_new"
fi

# Patches for files outside sapi/ — a departure from "upstream untouched", so
# loudly. Rules and validity windows: patches/README.md
PHPVER=$(awk -F'"' '/PHP_VERSION /{print $2}' "$PHPSRC/main/php_version.h" 2>/dev/null)
PHPMINOR=$(echo "$PHPVER" | cut -d. -f1,2)
# Patches form a stack and can touch the same region (0002 and 0003 both sit
# at accept()). Then the "is it already applied" test per patch lies: the
# reverse dry-run of 0002 fails, because 0003 sits on top of it. The decision
# is therefore made once, for the whole stack: either the tree is untouched
# and we apply everything in order, or the whole stack comes off in reverse
# from the copy of touched files (= already applied), or ERROR.
PATCHES=""
for p in "$REPO"/patches/*.patch; do
  [ -f "$p" ] || continue
  name=$(basename "$p")
  # versioned variant overrides the generic one
  [ -f "$REPO/patches/php-$PHPMINOR/$name" ] && p="$REPO/patches/php-$PHPMINOR/$name"
  PATCHES="$PATCHES $p"
done
PATCHED=0
if [ -n "$PATCHES" ]; then
  FIRST=${PATCHES%% *}; FIRST=${PATCHES# }; FIRST=${FIRST%% *}
  # Order matters: FIRST try forward. Applying in reverse to an untouched tree
  # can also return success (BSD patch), so testing "-R" first would silently
  # produce a binary without the patch and a message claiming it is there.
  if patch -d "$PHPSRC" -p1 --dry-run --forward --silent < "$FIRST" >/dev/null 2>&1; then
    for p in $PATCHES; do
      name=$(basename "$p")
      # Apply each patch immediately. Later patches may deliberately use
      # context introduced by earlier ones, so probing every patch against the
      # untouched tree reports false failures (0004/0005 on PHP 8.6).
      if ! patch -d "$PHPSRC" -p1 --forward --silent < "$p" >/dev/null 2>&1; then
        echo "ERROR: patch does not apply to PHP $PHPVER: $name" >&2
        echo "      see patches/README.md — either upstream merged it (remove it)," >&2
        echo "      or a patches/php-$PHPMINOR/$name variant is needed" >&2
        exit 1
      fi
      echo "  ! patch applied onto upstream: $name"
      PATCHED=$((PATCHED + 1))
    done
  else
    TMP=$(mktemp -d)
    # the file name ends at the first whitespace (diff -u appends the date there)
    for f in $(cat $PATCHES | sed -n 's|^+++ b/\([^[:space:]]*\).*|\1|p' | sort -u); do
      mkdir -p "$TMP/$(dirname "$f")"
      cp "$PHPSRC/$f" "$TMP/$f"
    done
    REVERSED=""
    for p in $PATCHES; do REVERSED="$p $REVERSED"; done
    OK=1
    for p in $REVERSED; do
      patch -d "$TMP" -p1 -R --forward --silent < "$p" >/dev/null 2>&1 || { OK=0; break; }
    done
    rm -rf "$TMP"
    if [ "$OK" = 1 ]; then
      for p in $PATCHES; do
        echo "  ! patch was already applied: $(basename "$p")"
        PATCHED=$((PATCHED + 1))
      done
    else
      echo "ERROR: patches do not apply to PHP $PHPVER and the tree does not look untouched" >&2
      echo "      ($(echo $PATCHES | wc -w | tr -d ' ') patches, first: $(basename "$FIRST"))" >&2
      echo "      see patches/README.md — either upstream merged one of them (remove it)," >&2
      echo "      or a patches/php-$PHPMINOR/<name> variant is needed, or the tree" >&2
      echo "      has foreign changes in these files (git status in $PHPSRC)" >&2
      exit 1
    fi
  fi
fi

echo "sapi/fpmng ready."
if [ "$PATCHED" -gt 0 ]; then
  echo "  WARNING: upstream was modified by $PATCHED patch(es) (see above)"
else
  echo "  upstream untouched — only sapi/fpmng/ was created"
fi
echo "  sources from upstream + ours: $(echo "$SOURCES" | wc -l | tr -d ' ')"
echo "  our files:"
(cd "$REPO/sapi/fpmng" && find . -type f | sed 's|^\./|    |' | sort)
if [ -d "$REPO/ext/fpmng_metrics" ]; then
  (cd "$REPO/ext/fpmng_metrics" && find . -type f | sed 's|^|    ext/fpmng_metrics/|' | sort)
fi

if [ -n "$SOURCES_CHANGED" ]; then
  echo
  echo "================================================================"
  echo "WARNING: the sapi/fpmng source file list changed:"
  echo "$SOURCES_CHANGED" | sed 's/^/    /'
  echo
  echo "The existing build directory has a frozen object list and will NOT"
  echo "see it. Before building, run:"
  echo
  echo "    cd $PHPSRC && ./buildconf --force"
  echo "    cd <build-directory> && ./config.nice && make"
  echo
  echo "Skipping this ends in a link error — or, worse, a binary without the"
  echo "new code that builds without a word of complaint."
  echo "================================================================"
fi
