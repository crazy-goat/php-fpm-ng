#!/bin/sh
# The refusals of build/libphp-build.sh, each one triggered on purpose
# (issue #422). The supported contract (#419) is PHP 8.5, NTS, Linux. A build
# outside it has to stop before the first compile, and the message has to say
# why. A guard that is never exercised can rot into a no-op without anyone
# noticing, so every guard gets a case here.
#
# Hermetic: no SDK, no compiler, no network. A fake php-config describes the
# SDK, and a fake uname describes the kernel. That makes this runnable in the
# CI checks job and on a macOS laptop alike.
#
# Usage: build/test-libphp-build-refusals.sh
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$REPO/build/libphp-build.sh
WORK=$(mktemp -d "${TMPDIR:-/tmp}/libphp-refusals.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

FAILED=0
CASES=0

# fake_sdk <dir> <version> <zts 0|1>: a php-config and the one header the
# build reads from the SDK before compiling anything.
fake_sdk() {
  mkdir -p "$1/bin" "$1/include/main"
  if [ "$3" = 1 ]; then
    printf '#define ZTS 1\n' > "$1/include/main/php_config.h"
  else
    printf '/* NTS */\n' > "$1/include/main/php_config.h"
  fi
  cat > "$1/bin/php-config" <<EOF
#!/bin/sh
case "\$1" in
--includes) echo "-I$1/include -I$1/include/main" ;;
--version) echo "$2" ;;
--include-dir) echo "$1/include" ;;
*) exit 1 ;;
esac
EOF
  chmod +x "$1/bin/php-config"
}

# fake_uname <dir> <kernel>
fake_uname() {
  mkdir -p "$1"
  printf '#!/bin/sh\necho %s\n' "$2" > "$1/uname"
  chmod +x "$1/uname"
}

fake_sdk "$WORK/sdk85" 8.5.4 0
fake_sdk "$WORK/sdk84" 8.4.16 0
fake_sdk "$WORK/sdk86" 8.6.0-dev 0
fake_sdk "$WORK/sdkzts" 8.5.4 1
fake_uname "$WORK/linux" Linux
fake_uname "$WORK/darwin" Darwin

# A PATH with the tools the script needs before it looks for php-config, and
# no php-config of any name: the autodetection has to come up empty.
mkdir -p "$WORK/bare"
for t in sh dirname mkdir cut grep cat; do
  ln -s "$(command -v "$t")" "$WORK/bare/$t"
done
cp "$WORK/linux/uname" "$WORK/bare/uname"

# A compiler that only records that it was called. Every probe and compile
# goes through $CC, so "the spy was never called" is "nothing was compiled".
cat > "$WORK/cc-spy" <<EOF
#!/bin/sh
echo "\$*" >> "$WORK/cc-calls"
exit 0
EOF
chmod +x "$WORK/cc-spy"
SPY=$WORK/cc-spy

# expect <name> <message fragment> <env and command...>
# The command must exit non-zero, print the fragment, and must not have
# called the compiler: a refusal that happens after compiling is not a
# refusal.
expect() {
  name=$1
  want=$2
  shift 2
  CASES=$((CASES + 1))
  rm -f "$WORK/cc-calls"
  if env "$@" > "$WORK/log-$CASES" 2>&1; then
    echo "FAIL: $name: the build succeeded; it should have refused"
    FAILED=$((FAILED + 1))
    return 0
  fi
  if ! grep -qF -- "$want" "$WORK/log-$CASES"; then
    echo "FAIL: $name: refused, but without the message '$want'. The output was:"
    sed 's/^/    /' "$WORK/log-$CASES"
    FAILED=$((FAILED + 1))
    return 0
  fi
  if [ -n "$REFUSAL" ] && [ -e "$WORK/cc-calls" ]; then
    echo "FAIL: $name: refused, but only after calling the compiler"
    FAILED=$((FAILED + 1))
    return 0
  fi
  echo "ok: $name"
}

L="PATH=$WORK/linux:$PATH"
REFUSAL=yes
OK_SDK="PHP_CONFIG=$WORK/sdk85/bin/php-config"

expect "the old two-argument call" "takes no php-src tree any more" \
  "$L" "$OK_SDK" CC="$SPY" sh "$BUILD" /some/php-src "$WORK/out-legacy"
expect "a kernel other than Linux" "supports Linux only" \
  "PATH=$WORK/darwin:$PATH" "$OK_SDK" CC="$SPY" sh "$BUILD" "$WORK/out-darwin"
expect "no php-config anywhere" "no php-config found" \
  "PATH=$WORK/bare" CC="$SPY" sh "$BUILD" "$WORK/out-bare"
expect "PHP_CONFIG naming nothing" "is not an executable" \
  "$L" PHP_CONFIG="$WORK/nonexistent/php-config" CC="$SPY" sh "$BUILD" "$WORK/out-noexec"
expect "a PHP 8.4 SDK" "supports PHP 8.5 only" \
  "$L" PHP_CONFIG="$WORK/sdk84/bin/php-config" CC="$SPY" sh "$BUILD" "$WORK/out-84"
expect "a PHP 8.6 SDK" "supports PHP 8.5 only" \
  "$L" PHP_CONFIG="$WORK/sdk86/bin/php-config" CC="$SPY" sh "$BUILD" "$WORK/out-86"
expect "a ZTS SDK" "is a ZTS (thread-safe) build of PHP" \
  "$L" PHP_CONFIG="$WORK/sdkzts/bin/php-config" CC="$SPY" sh "$BUILD" "$WORK/out-zts"
expect "no C compiler" "no C compiler" \
  "$L" "$OK_SDK" CC=no-such-cc-422 sh "$BUILD" "$WORK/out-nocc"
expect "a toggle that is not 0 or 1" "FPMNG_TLS must be 0 or 1, not 'yes'" \
  "$L" "$OK_SDK" CC="$SPY" FPMNG_TLS=yes sh "$BUILD" "$WORK/out-toggle"
expect "the debug clock toggle checked too" "FPMNG_DEBUG_CLOCK must be 0 or 1" \
  "$L" "$OK_SDK" CC="$SPY" FPMNG_DEBUG_CLOCK=on sh "$BUILD" "$WORK/out-clock"
expect "ACME without TLS" "FPMNG_ACME=1 needs FPMNG_TLS=1" \
  "$L" "$OK_SDK" CC="$SPY" FPMNG_TLS=0 FPMNG_ACME=1 sh "$BUILD" "$WORK/out-acme"
REFUSAL=

# The positive control: the same fakes, inside the contract, get past every
# guard above. The spy makes each compile probe "succeed" without producing a
# program, so the build stops at the first probe it runs, the /proc/<pid>/mem
# one. Reaching that message, with the compiler called, proves no guard
# refused a supported setup.
expect "a supported setup passes every guard" "pread() on /proc/<pid>/mem does not work here" \
  "$L" "$OK_SDK" CC="$SPY" FPMNG_TLS=1 FPMNG_ACME=1 sh "$BUILD" "$WORK/out-good"
[ -e "$WORK/cc-calls" ] || { echo "FAIL: the supported setup never reached the compiler"; FAILED=$((FAILED + 1)); }
for refusal in "takes no php-src" "Linux only" "no php-config" "8.5 only" "ZTS" "no C compiler" "must be 0 or 1" "needs FPMNG_TLS"; do
  if grep -qF -- "$refusal" "$WORK/log-$CASES"; then
    echo "FAIL: the supported setup tripped the '$refusal' guard"
    FAILED=$((FAILED + 1))
  fi
done

echo "test-libphp-build-refusals.sh: $CASES cases, $FAILED failed"
[ "$FAILED" = 0 ]
