#!/bin/sh
# Audit what a compile actually read, from the compiler's own -MD output
# (issue #421).
#
#   build/audit-compile-deps.sh <dep-dir> <sdk-include-dir> <allowed-dir>...
#
# <dep-dir> holds the .d files of one build. Every file named in them must lie
# under the SDK include directory, under one of the allowed directories, or in
# the toolchain's system directories; anything else is a file the build
# consumed without this repository or the SDK accounting for it -- a php-src
# checkout on the include path, a header from another PHP build on the
# machine. Three further failures are specific:
#
#   - the SDK's main/fastcgi.h was read. It is the unpatched upstream header;
#     reading it means the build compiled against a FastCGI that is not the
#     one it links (patches/0001).
#   - any main/ header other than the vendored fastcgi.h came from outside the
#     SDK: header shadowing, the defect the old libphp build had.
#   - a file listed in third_party/php-src/MANIFEST (C or header) was read by
#     no translation unit. Either the inventory carries dead weight, or the
#     build is not compiling what it is supposed to.
#
# The report it prints is the evidence: how many files came from where.
set -eu

DEPS=${1:?usage: audit-compile-deps.sh <dep-dir> <sdk-include-dir> <allowed-dir>...}
SDK=${2:?missing sdk include dir}
shift 2
REPO=$(cd "$(dirname "$0")/.." && pwd)

fail() {
  echo "audit-compile-deps.sh: FAIL: $*" >&2
  exit 1
}

ls "$DEPS"/*.d >/dev/null 2>&1 || fail "no .d files in $DEPS"

# One dependency per line, canonical. A .d file is make syntax: "target: dep
# dep \" with continuation lines. The target and the backslashes go; every
# remaining word is a path. Paths are canonicalised so "events/../zlog.h"
# counts as zlog.h.
all=$(mktemp)
for d in "$DEPS"/*.d; do
  sed -e 's/^[^:]*://' -e 's/\\$//' "$d" | tr ' \t' '\n\n' | grep -v '^$'
done | sort -u | while read -r p; do
  ( cd "$(dirname "$p")" 2>/dev/null && echo "$(pwd -P)/$(basename "$p")" ) || echo "$p"
done | sort -u > "$all"

canon() { (cd "$1" && pwd -P); }
SDK_C=$(canon "$SDK")
allowed=
for a in "$@"; do allowed="$allowed $(canon "$a")"; done

: > "$all.bad"
while read -r p; do
  case "$p" in "$SDK_C"/*) continue ;; esac
  hit=
  for a in $allowed; do
    case "$p" in "$a"/*) hit=1; break ;; esac
  done
  [ -z "$hit" ] || continue
  # PHP headers of another installation -- /usr/include/php84/Zend/...,
  # /usr/local/include/php/... -- sit under system directories, and must not
  # pass as "system" for that.
  case "$p" in
    */php*/main/*|*/php*/Zend/*|*/php*/TSRM/*|*/php*/ext/*|*/php*/sapi/*)
      echo "$p" >> "$all.bad"; continue ;;
  esac
  case "$p" in
    /usr/include/*|/usr/lib/gcc/*|/usr/lib/clang/*|/usr/lib/llvm*|/usr/local/include/*) continue ;;
  esac
  echo "$p" >> "$all.bad"
done < "$all"
n_sdk=$(grep -c "^$SDK_C/" "$all" || true)
n_bad=$(wc -l < "$all.bad" | tr -d ' ')

problems=0
if [ "$n_bad" != 0 ]; then
  echo "  read from outside the SDK, the allowed directories and the system:" >&2
  sed 's/^/    /' "$all.bad" >&2
  problems=$((problems + 1))
fi
if grep -qx "$SDK_C/main/fastcgi.h" "$all"; then
  echo "  the SDK's unpatched main/fastcgi.h was read; the vendored one must come first" >&2
  problems=$((problems + 1))
fi
# main/*.h from anywhere but the SDK, except the vendored fastcgi.h.
shadow=$(grep -E '/main/[^/]*\.h$|/main/streams/' "$all" | grep -v "^$SDK_C/" | grep -v '/main/fastcgi\.h$' || true)
if [ -n "$shadow" ]; then
  echo "  php-src main/ headers read from outside the SDK (header shadowing):" >&2
  echo "$shadow" | sed 's/^/    /' >&2
  problems=$((problems + 1))
fi
# Every vendored C/header file read by something. The basename is matched
# under any allowed directory, because the build reads the assembled copy, not
# third_party/ itself.
unused=
for f in $(awk -F '\t' '!/^#/ && NF == 4 && $1 ~ /\.[ch]$/ { print $1 }' "$REPO/third_party/php-src/MANIFEST"); do
  rel=$f
  case "$f" in sapi/fpm/fpm/*) rel=sapi/fpmng/fpm/${f#sapi/fpm/fpm/} ;; esac
  grep -q "/$rel\$" "$all" || unused="$unused $f"
done
if [ -n "$unused" ]; then
  echo "  vendored but read by no translation unit:$unused" >&2
  problems=$((problems + 1))
fi

total=$(wc -l < "$all" | tr -d ' ')
n_allowed=$(for a in $allowed; do grep "^$a/" "$all"; done | sort -u | wc -l | tr -d ' ')
n_sys=$((total - n_sdk - n_allowed - n_bad))
echo "audit-compile-deps.sh: $(ls "$DEPS"/*.d | wc -l | tr -d ' ') translation units read $total files: $n_allowed from the repository's assembled tree, $n_sdk from the SDK ($SDK_C), $n_sys from the system, $n_bad from elsewhere"
for a in $allowed; do
  echo "  $(grep -c "^$a/" "$all" || true) under $a"
done
rm -f "$all" "$all.bad"
[ "$problems" = 0 ] || fail "$problems problem(s) in what the compile consumed (see above)"
echo "audit-compile-deps.sh: PASS"
