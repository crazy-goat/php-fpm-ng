#!/bin/sh
# Exercise the libphp version guard (build/libphp/libphp_abi_check.c, issue
# #220) in all of its outcomes (issue #422) and the struct-size check (issue
# #994).
#
# No pair of packages that exists can reach the two interesting branches. An
# 8.5 binary cannot be built against 8.4 headers, and no distribution ships two
# patch releases of one minor side by side. So this compiles the real guard
# against the installed SDK, links it with a main() that references the same
# globals the server does and the real libphp, and moves only the compiled-
# against side through the test seams (FPMNG_BUILT_PHP_VERSION_ID,
# FPMNG_BUILT_EXECUTOR_GLOBALS_SIZE, FPMNG_BUILT_COMPILER_GLOBALS_SIZE). The
# loaded side is always the real php_version_id() and the real symbol sizes of
# the libphp the dynamic loader picked.
#
#   another minor                 -> FATAL on stderr, exit 1, main() never runs
#   another patch level           -> one notice on stderr, main() runs
#   the same version              -> silent, main() runs
#   another executor_globals size -> FATAL on stderr, exit 1, main() never runs
#   another compiler_globals size -> FATAL on stderr, exit 1, main() never runs
#   a loaded libphp that is larger than the executable's copy of executor_globals
#                                 -> FATAL on stderr, exit 1, main() never runs
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

# The real struct sizes of this SDK, to build the fake ones from.
cat > "$WORK/sizes.c" <<'EOF'
#include "php.h"
#include "zend_globals.h"
#include <stdio.h>
int main(void) { printf("%zu %zu\n", sizeof(zend_executor_globals), sizeof(zend_compiler_globals)); return 0; }
EOF
# shellcheck disable=SC2046
"$CC" -D_GNU_SOURCE $("$PHP_CONFIG" --includes) "$WORK/sizes.c" -o "$WORK/sizes"
SIZES=$("$WORK/sizes")
EXEC_SIZE=${SIZES% *}
COMP_SIZE=${SIZES#* }

# The banner line stands in for `php-fpm-ng -v`, which build/check-libphp-skew.sh
# insists on seeing before it believes that nothing was printed (issue #557).
# The stub takes the addresses of both globals, as the server does, so the
# executable gets the same copy relocations: the guard must read the libphp
# file and not the executable's own copy (issue #994).
cat > "$WORK/main.c" <<'EOF'
#include "php.h"
#include "zend_globals.h"
#include <stdio.h>
int main(void)
{
	void *volatile refs[2];
	refs[0] = &executor_globals;
	refs[1] = &compiler_globals;
	puts("PHP 0.0.0 (stub)");
	return 42;
}
EOF
# shellcheck disable=SC2046
"$CC" -D_GNU_SOURCE -O2 -c "$WORK/main.c" $("$PHP_CONFIG" --includes) -o "$WORK/main.o"

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

echo "test-libphp-abi-guard.sh: SDK $VER ($PHP_CONFIG), libphp $LIBPHP, executor_globals $EXEC_SIZE bytes, compiler_globals $COMP_SIZE bytes"

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
echo "ok: the same version ($REAL_ID) and the same struct sizes run silently"
"$REPO/build/check-libphp-skew.sh" "$WORK/same" > /dev/null || fail "check-libphp-skew.sh refused a binary with no skew"
echo "ok: check-libphp-skew.sh accepts it"

# Same version, a struct size that differs: the Ubuntu case of issue #994, where
# the patch keeps 8.5.4 and adds 24 bytes to executor_globals. Each struct has its
# own case, so a check that looks at only one of them fails here.
EXEC_FAKE=$((EXEC_SIZE + 24))
build exec "-DFPMNG_BUILT_EXECUTOR_GLOBALS_SIZE=$EXEC_FAKE"
run exec
[ "$RC" = 1 ] || fail "built with another executor_globals size: exit $RC, expected 1 before main() (stderr: $ERR)"
echo "$ERR" | grep -q "php-fpm-ng: FATAL: this binary was built against PHP $VER headers, with executor_globals of $EXEC_FAKE bytes and compiler_globals of $COMP_SIZE bytes, but the libphp it loaded (PHP $VER) has $EXEC_SIZE and $COMP_SIZE bytes" ||
  fail "built with another executor_globals size: no FATAL naming both sizes (stderr: $ERR)"
echo "ok: another executor_globals size is refused before main() (exit 1, FATAL names $EXEC_FAKE and $EXEC_SIZE)"

COMP_FAKE=$((COMP_SIZE + 8))
build comp "-DFPMNG_BUILT_COMPILER_GLOBALS_SIZE=$COMP_FAKE"
run comp
[ "$RC" = 1 ] || fail "built with another compiler_globals size: exit $RC, expected 1 before main() (stderr: $ERR)"
echo "$ERR" | grep -q "FATAL: this binary was built against PHP $VER headers, with executor_globals of $EXEC_SIZE bytes and compiler_globals of $COMP_FAKE bytes, but the libphp it loaded (PHP $VER) has $EXEC_SIZE and $COMP_SIZE bytes" ||
  fail "built with another compiler_globals size: no FATAL naming both sizes (stderr: $ERR)"
echo "ok: another compiler_globals size is refused before main() (exit 1, FATAL names $COMP_FAKE and $COMP_SIZE)"

# The executable keeps its own copy of executor_globals (a copy relocation). Link
# against a stand-in libphp of the real size, then run it against a stand-in that
# is 24 bytes larger. A guard that reads the copy sees the real size and passes.
# Only a guard that reads the loaded file refuses this.
stand() {
  mkdir "$1"
  cat > "$1/libphp.c" <<EOF
const char *php_version(void) { return "$VER"; }
unsigned int php_version_id(void) { return $REAL_ID; }
char executor_globals[$2] __attribute__((aligned(8)));
char compiler_globals[$COMP_SIZE] __attribute__((aligned(8)));
EOF
  "$CC" -shared -fPIC -Wl,-soname,libphp.so "$1/libphp.c" -o "$1/libphp.so"
}
stand "$WORK/linked" "$EXEC_SIZE"
stand "$WORK/loaded" "$EXEC_FAKE"
"$CC" -D_GNU_SOURCE -O2 -Wno-deprecated-declarations $("$PHP_CONFIG" --includes) \
  -c "$REPO/build/libphp/libphp_abi_check.c" -o "$WORK/copy.o"
"$CC" "$WORK/main.o" "$WORK/copy.o" -L"$WORK/linked" -lphp -o "$WORK/copy"
set +e
LD_LIBRARY_PATH="$WORK/loaded" "$WORK/copy" > /dev/null 2> "$WORK/copy.err"
RC=$?
set -e
ERR=$(cat "$WORK/copy.err")
[ "$RC" = 1 ] || fail "loaded libphp with another executor_globals size: exit $RC, expected 1 before main() (stderr: $ERR)"
echo "$ERR" | grep -q "FATAL: this binary was built against PHP $VER headers, with executor_globals of $EXEC_SIZE bytes and compiler_globals of $COMP_SIZE bytes, but the libphp it loaded (PHP $VER) has $EXEC_FAKE and $COMP_SIZE bytes" ||
  fail "loaded libphp with another executor_globals size: no FATAL naming both sizes (stderr: $ERR)"
echo "ok: a libphp loaded with another executor_globals size is refused although the executable has its own copy (FATAL names $EXEC_SIZE and $EXEC_FAKE)"
