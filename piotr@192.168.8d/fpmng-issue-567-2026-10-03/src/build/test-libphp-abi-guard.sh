#!/bin/sh
# Exercise the libphp version guard (build/libphp/libphp_abi_check.c, issue
# #220) in all three of its outcomes (issue #422).
#
# No pair of packages that exists can reach the two interesting branches. An
# 8.5 binary cannot be built against 8.4 headers, and no distribution ships two
# patch releases of one minor side by side. So this compiles the real guard
# against the installed SDK, links it with an empty main() and the real
# libphp, and moves only the compiled-against side through the test seam
# (FPMNG_BUILT_PHP_VERSION_ID). The loaded side is always the real
# php_version_id() of the libphp the dynamic loader picked.
#
#   another minor        -> FATAL on stderr, exit 1, main() never runs
#   another patch level  -> one notice on stderr, main() runs
#   the same version     -> silent, main() runs
#
# Needs a PHP 8.5 SDK (php8.5-dev and libphp8.5-embed, or php85-dev and
# php85-embed) and a C compiler.
#
# Usage: build/test-libphp-abi-guard.sh
set -eu

fail() { echo "test-libphp-abi-guard.sh: FAIL: $*" >&2; exit 1; }

REPO=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${PHP_CONFIG:-}" ]; then
  for c in php-config8.5 php-config85 php-config; do
    command -v "$c" >/dev/null 2>&1 && { PHP_CONFIG=$c; break; }
  done
fi
[ -n "${PHP_CONFIG:-}" ] || fail "no php-config found; this test needs a PHP 8.5 SDK"
CC=${CC:-gcc}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/libphp-abi.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

VER=$("$PHP_CONFIG" --version)
MM=$(echo "$VER" | cut -d. -f1,2)
# The same lookup build/libphp-build.sh makes: Ubuntu ships
# /usr/lib/libphp8.5.so, Alpine /usr/lib/php85/libphp.so.
LIBPHP=
for dir in "$(dirname "$("$PHP_CONFIG" --extension-dir)")" "$("$PHP_CONFIG" --prefix)/lib" /usr/lib /usr/lib64; do
  for name in "php$MM" php; do
    if [ -e "$dir/lib$name.so" ]; then LIBPHP=$dir/lib$name.so; break 2; fi
  done
done
[ -n "$LIBPHP" ] || fail "no libphp shared object found for $PHP_CONFIG; install the embed package"

# The real version split into its parts, to build the two fake ones from.
MAJOR=$(echo "$VER" | cut -d. -f1)
MINOR=$(echo "$VER" | cut -d. -f2)
PATCH=$(echo "$VER" | cut -d. -f3 | sed 's/[^0-9].*//')
REAL_ID=$((MAJOR * 10000 + MINOR * 100 + PATCH))
OTHER_MINOR_ID=$((MAJOR * 10000 + (MINOR - 1) * 100 + PATCH))
if [ "$PATCH" = 0 ]; then OTHER_PATCH=1; else OTHER_PATCH=$((PATCH - 1)); fi
OTHER_PATCH_ID=$((MAJOR * 10000 + MINOR * 100 + OTHER_PATCH))

# The banner line stands in for `php-fpm-ng -v`, which build/check-libphp-skew.sh
# insists on seeing before it believes that nothing was printed (issue #557).
printf '#include <stdio.h>\nint main(void) { puts("PHP 0.0.0 (stub)"); return 42; }\n' > "$WORK/main.c"
"$CC" -c "$WORK/main.c" -o "$WORK/main.o"

# build <name> [-D...]: the guard object plus main, linked like the server is.
build() {
  name=$1
  shift
  # shellcheck disable=SC2046
  "$CC" -D_GNU_SOURCE -O2 -Wno-deprecated-declarations "$@" $("$PHP_CONFIG" --includes) \
    -c "$REPO/build/libphp/libphp_abi_check.c" -o "$WORK/$name.o"
  "$CC" "$WORK/main.o" "$WORK/$name.o" "$LIBPHP" -Wl,-rpath,"$(dirname "$LIBPHP")" -o "$WORK/$name"
}

# run <name>: sets RC and ERR.
run() {
  set +e
  "$WORK/$1" > /dev/null 2> "$WORK/$1.err"
  RC=$?
  set -e
  ERR=$(cat "$WORK/$1.err")
}

echo "test-libphp-abi-guard.sh: SDK $VER ($PHP_CONFIG), libphp $LIBPHP"

build minor "-DFPMNG_BUILT_PHP_VERSION_ID=$OTHER_MINOR_ID" "-DFPMNG_BUILT_PHP_VERSION=\"$MAJOR.$((MINOR - 1)).$PATCH\""
run minor
[ "$RC" = 1 ] || fail "built for another minor: exit $RC, expected 1 before main() (stderr: $ERR)"
echo "$ERR" | grep -q "php-fpm-ng: FATAL: this binary was built against PHP $MAJOR.$((MINOR - 1)).$PATCH headers, but the libphp it loaded is PHP $VER" ||
  fail "built for another minor: no FATAL naming both versions (stderr: $ERR)"
echo "ok: another minor is refused before main() (exit 1, FATAL names $MAJOR.$((MINOR - 1)).$PATCH and $VER)"

build patch "-DFPMNG_BUILT_PHP_VERSION_ID=$OTHER_PATCH_ID" "-DFPMNG_BUILT_PHP_VERSION=\"$MAJOR.$MINOR.$OTHER_PATCH\""
run patch
[ "$RC" = 42 ] || fail "built for another patch level: exit $RC, expected main()'s 42 (stderr: $ERR)"
[ "$(echo "$ERR" | grep -c 'notice: built against')" = 1 ] ||
  fail "built for another patch level: expected exactly one notice (stderr: $ERR)"
echo "$ERR" | grep -q "built against PHP $MAJOR.$MINOR.$OTHER_PATCH, running on libphp $VER (patch-level difference, supported)" ||
  fail "built for another patch level: the notice does not name both versions (stderr: $ERR)"
echo "ok: another patch level runs, with one notice naming $MAJOR.$MINOR.$OTHER_PATCH and $VER"
# The gate's own check (issue #557) must refuse exactly this binary, and name both versions.
if "$REPO/build/check-libphp-skew.sh" "$WORK/patch" > "$WORK/skew.out" 2>&1; then
  fail "check-libphp-skew.sh accepted a binary built against another patch level"
fi
grep -q "built against PHP $MAJOR.$MINOR.$OTHER_PATCH and runs on libphp $VER" "$WORK/skew.out" ||
  fail "check-libphp-skew.sh did not name both versions: $(cat "$WORK/skew.out")"
echo "ok: check-libphp-skew.sh refuses it, naming $MAJOR.$MINOR.$OTHER_PATCH and $VER"

build same
run same
[ "$RC" = 42 ] || fail "built for the same version: exit $RC, expected main()'s 42 (stderr: $ERR)"
[ -z "$ERR" ] || fail "built for the same version: expected silence, got: $ERR"
echo "ok: the same version ($REAL_ID) runs silently"
"$REPO/build/check-libphp-skew.sh" "$WORK/same" > /dev/null || fail "check-libphp-skew.sh refused a binary with no skew"
echo "ok: check-libphp-skew.sh accepts it"
