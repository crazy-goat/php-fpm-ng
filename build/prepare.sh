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
# at the end.
OLD_SOURCES=""
if [ -f "$PHPSRC/sapi/fpmng/config.m4" ]; then
  # Only the PHP_FPMNG_FILES="..." block — config.m4 also mentions other
  # files (fpm_systemd.c, fpm_trace.c, www.c under conditions) that do not
  # belong to it.
  OLD_SOURCES=$(sed -n '/PHP_FPMNG_FILES="/,/^[[:space:]]*"[[:space:]]*$/p' \
      "$PHPSRC/sapi/fpmng/config.m4" \
    | grep -oE 'fpm/[A-Za-z0-9_/]+\.c' | sort -u)
fi

rm -rf "$PHPSRC/sapi/fpmng"
cp -r "$PHPSRC/sapi/fpm" "$PHPSRC/sapi/fpmng"

# Keep the upstream tests in the copied SAPI. The runner supplies the binary path
# through TEST_PHP_FPM_EXECUTABLE, so the tests do not need to be forked here.

# zlog.h is the one upstream header we EXTEND rather than replace (issue #130:
# sapi/fpmng/fpm/zlog.h routes zlog() through fpmng_zlog_ex()). Keeping a copy
# under a second name lets our zlog.h include it, so the struct definitions and
# the prototypes still come from THIS php-src. Owning the header outright would
# freeze them: struct zlog_stream gained two bit-fields in 8.5, so a copy taken
# from one branch and compiled against another silently describes a different
# object. The copy is made BEFORE our files land on top, because ours overwrites
# the original name.
cp "$PHPSRC/sapi/fpm/fpm/zlog.h" "$PHPSRC/sapi/fpmng/fpm/zlog_upstream.h"

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

# Split into three groups: tls and acme go under --enable-fpmng-tls /
# --enable-fpmng-acme (both default "no"); the rest is always built. Issue
# #373 removed the fourth and fifth groups this used to have (one for the
# fiber executor and its shared-state layer, one for the async executor's own
# source file) along with the sources themselves -- they live on branch async
# now.
# Matching by NAME PREFIX, not by enumerating files. Enumeration would undo
# the whole point of this script: the source list must come from 'find' so a
# new file needs no edit.
#
# The TLS group is why the prefix is fpm_tls_ and not the older fpm_*_tls*
# naming (issue #280): fpm_http_direct_tls.c must stay in EVERY build — it
# holds the config validation that refuses http.tls_cert in a build without
# TLS, and four callers call into it with no #ifdef of their own. A pattern
# that matched "tls anywhere in the name" would take it out of the default
# binary and break the link. So the rule is the file name: fpm_tls_*.c is
# code that needs OpenSSL, everything else is code that does not.
TLS_PATTERN='^fpm/fpm_tls_[A-Za-z0-9_]*\.c$'
# The ACME group needs no such care: nothing outside it is named fpm_acme_*,
# and the three callers that reach into it (fpm.c, fpm_pool_script.c,
# fpm_http.c) go through stubs in fpm_acme_challenge.h when the flag is off
# (issue #281).
ACME_PATTERN='^fpm/fpm_acme_[A-Za-z0-9_]*\.c$'

BASE_SOURCES=$(echo "$SOURCES" | grep -Ev "$TLS_PATTERN" | grep -Ev "$ACME_PATTERN")
TLS_SOURCES=$(echo "$SOURCES" | grep -E "$TLS_PATTERN")
ACME_SOURCES=$(echo "$SOURCES" | grep -E "$ACME_PATTERN")

[ -n "$TLS_SOURCES" ] || { echo "no fpm_tls_*.c files found in the source list" >&2; exit 1; }
[ -n "$ACME_SOURCES" ] || { echo "no fpm_acme_*.c files found in the source list" >&2; exit 1; }

BASE_LIST=$(echo "$BASE_SOURCES" | sed 's/$/ \\/' | sed 's/^/    /')
TLS_LIST=$(echo "$TLS_SOURCES" | sed 's/$/ \\/' | sed 's/^/    /')
ACME_LIST=$(echo "$ACME_SOURCES" | sed 's/$/ \\/' | sed 's/^/    /')

awk -v base="$BASE_LIST" -v tls="$TLS_LIST" -v acme="$ACME_LIST" '{
    gsub(/@FPMNG_SOURCES@/, "\n" base "\n  ");
    gsub(/@FPMNG_TLS_SOURCES@/, "\n" tls "\n  ");
    gsub(/@FPMNG_ACME_SOURCES@/, "\n" acme "\n  ");
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

echo "sapi/fpmng ready."
echo "  upstream untouched — only sapi/fpmng/ was created"
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
