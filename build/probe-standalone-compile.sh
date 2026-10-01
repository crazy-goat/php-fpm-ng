#!/bin/sh
# Compile and link the SAPI from this repository and a PHP SDK alone, and show
# what the compiler read (issue #421).
#
#   build/probe-standalone-compile.sh <outdir>
#   PHP_CONFIG   php-config to build against; autodetected like libphp-build.sh
#   FPMNG_TLS    1 (default) or 0
#   FPMNG_ACME   1 (default) or 0; needs FPMNG_TLS=1
#
# This is the sufficiency evidence for third_party/php-src/, not the build
# interface: build/libphp-build.sh is still the build, and it still wants a
# prepared php-src tree until issue #422 moves it onto build/standalone-tree.sh.
# The defines and flags below mirror libphp-build.sh on purpose, so that the
# probe compiles the same translation units the shipped build compiles.
#
# No php-src tree is involved: the sources are build/standalone-tree.sh's
# assembly of sapi/fpmng/fpm/ and third_party/php-src/, everything else comes
# from `php-config --includes`. Every compile runs with -MD, and
# build/audit-compile-deps.sh then checks the dependency files: each file read
# is in the assembled tree, the repository, the SDK or the system; the SDK's
# unpatched fastcgi.h was not read; no main/ header was shadowed; and every
# vendored file was read by something. The link step closes the argument from
# the other side: an inventory missing a translation unit fails here with an
# undefined reference, which a compile-only probe would not notice.
#
# Writes <outdir>/commands.log (every command line, verbatim), <outdir>/dep/
# (the .d files) and <outdir>/php-fpm-ng.
set -eu

fail() {
  echo "probe-standalone-compile.sh: FAIL: $*" >&2
  exit 1
}

REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?usage: build/probe-standalone-compile.sh <outdir>}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
FPMNG_TLS=${FPMNG_TLS:-1}
FPMNG_ACME=${FPMNG_ACME:-1}
[ "$FPMNG_ACME" = 0 ] || [ "$FPMNG_TLS" = 1 ] || fail "FPMNG_ACME=1 needs FPMNG_TLS=1"

if [ -z "${PHP_CONFIG:-}" ]; then
  for c in php-config8.5 php-config85 php-config; do
    command -v "$c" >/dev/null 2>&1 && { PHP_CONFIG=$c; break; }
  done
fi
[ -n "${PHP_CONFIG:-}" ] || fail "no php-config found; install php8.5-dev (Debian/Ubuntu) or php85-dev (Alpine)"
INC_DIR=$("$PHP_CONFIG" --include-dir)
CORE_INC=$("$PHP_CONFIG" --includes)
PHP_VER=$("$PHP_CONFIG" --version)

"$REPO/build/vendor-php-src.sh" check

TREE=$OUT/tree
rm -rf "$OUT/obj" "$OUT/dep" "$OUT/compat"
mkdir -p "$OUT/obj" "$OUT/dep" "$OUT/compat"
"$REPO/build/standalone-tree.sh" "$TREE"
printf '#include "php_config.h"\n' > "$OUT/compat/config.h"

DEFS="-DHAVE_CONFIG_H -DHAVE_EPOLL=1 -DHAVE_SELECT=1 -DHAVE_BUILTIN_ATOMIC=1
-DHAVE_LQ_TCP_INFO=1 -DHAVE_TIMES=1 -DHAVE_CLEARENV=1 -DHAVE_CLOCK_GETTIME=1
-DHAVE_FPM_HTTP=1 -DFPMNG_LIBPHP_BUILD=1 -DPROC_MEM_FILE=\"mem\""
[ "$FPMNG_TLS" = 1 ] && DEFS="$DEFS -DHAVE_FPM_HTTP_TLS=1"
[ "$FPMNG_ACME" = 1 ] && DEFS="$DEFS -DHAVE_FPMNG_ACME=1"
DEFS=$(echo "$DEFS" | tr '\n' ' ')
# $TREE/main first: the vendored, patched fastcgi.h must win over the SDK's.
# It is the only file in that directory, so nothing else of main/ can shadow
# the SDK.
INC="-I$TREE/main $CORE_INC -I$TREE/sapi/fpmng -I$TREE/sapi/fpmng/fpm -I$REPO/ext/fpmng_metrics -I$REPO/build/libphp -I$OUT/compat"
CFLAGS="-D_GNU_SOURCE -O2 -g -fno-strict-aliasing -Wno-deprecated-declarations"
CC=${CC:-gcc}

: > "$OUT/commands.log"
echo "# $(uname -sm); $($CC --version | head -n 1); $PHP_CONFIG -> PHP $PHP_VER, $INC_DIR" >> "$OUT/commands.log"
echo "# FPMNG_TLS=$FPMNG_TLS FPMNG_ACME=$FPMNG_ACME" >> "$OUT/commands.log"

# Every .c of the assembled tree, minus the TLS and ACME groups when they are
# off (the same name-prefix rule as build/prepare.sh), plus the two files that
# are not SAPI sources.
sources() {
  (cd "$TREE/sapi/fpmng" && find fpm -name '*.c' | LC_ALL=C sort) | while read -r f; do
    case "$(basename "$f")" in
      fpm_tls_*) [ "$FPMNG_TLS" = 1 ] || continue ;;
      fpm_acme_*) [ "$FPMNG_ACME" = 1 ] || continue ;;
    esac
    echo "$TREE/sapi/fpmng/$f"
  done
  echo "$TREE/main/fastcgi.c"
  echo "$REPO/ext/fpmng_metrics/fpmng_metrics.c"
  echo "$REPO/build/libphp/libphp_abi_check.c"
}

OBJS=
n=0
for s in $(sources); do
  base=$(echo "${s#"$TREE"/}" | sed "s|^$REPO/||" | tr /. __)
  cmd="$CC $CFLAGS $DEFS $INC -MD -MF $OUT/dep/$base.d -c $s -o $OUT/obj/$base.o"
  echo "$cmd" >> "$OUT/commands.log"
  # shellcheck disable=SC2086
  $cmd >> "$OUT/compile.log" 2>&1 || { tail -n 20 "$OUT/compile.log" >&2; fail "compile failed: $s"; }
  OBJS="$OBJS $OUT/obj/$base.o"
  n=$((n + 1))
done

PHP_MM=$(echo "$PHP_VER" | cut -d. -f1,2)
LIBPHP_DIR= LIBPHP_NAME=
for dir in "$(dirname "$("$PHP_CONFIG" --extension-dir)")" "$("$PHP_CONFIG" --prefix)/lib" /usr/lib /usr/lib64; do
  for name in "php$PHP_MM" php; do
    if [ -e "$dir/lib$name.so" ]; then LIBPHP_DIR=$dir; LIBPHP_NAME=$name; break 2; fi
  done
done
[ -n "$LIBPHP_NAME" ] || fail "no libphp shared object found for $PHP_CONFIG"
EXTRA_LIBS=
grep -q '^#define HAVE_FPM_ACL' "$INC_DIR/main/php_config.h" && EXTRA_LIBS=-lacl
[ "$FPMNG_TLS" = 1 ] && EXTRA_LIBS="$EXTRA_LIBS -levent_openssl -lssl -lcrypto"
for l in dl rt pthread; do
  echo 'int main(void){return 0;}' | $CC -x c - "-l$l" -o /dev/null 2>/dev/null && EXTRA_LIBS="$EXTRA_LIBS -l$l"
done
cmd="$CC -o $OUT/php-fpm-ng $OBJS -L$LIBPHP_DIR -Wl,-rpath,$LIBPHP_DIR -l$LIBPHP_NAME -levent $EXTRA_LIBS -lm -Wl,-E"
echo "$cmd" >> "$OUT/commands.log"
# shellcheck disable=SC2086
$cmd > "$OUT/link.log" 2>&1 || { grep 'undefined reference' "$OUT/link.log" | head -n 20 >&2; fail "link failed"; }
"$OUT/php-fpm-ng" -v | head -n 1

"$REPO/build/audit-compile-deps.sh" "$OUT/dep" "$INC_DIR" "$TREE" "$REPO/ext/fpmng_metrics" "$REPO/build/libphp" "$OUT/compat"
echo "probe-standalone-compile.sh: PASS ($n translation units, PHP $PHP_VER, FPMNG_TLS=$FPMNG_TLS FPMNG_ACME=$FPMNG_ACME) -> $OUT"
