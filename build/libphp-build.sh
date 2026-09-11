#!/bin/sh
# Build php-fpm-ng against the distribution's libphp instead of compiling
# php-src (issue #212, from spike #198 -- docs/spike-libphp-link-report.md).
#
# WHAT THIS BUILD IS. For `pool.type = fastcgi` and `pool.type = http-direct`
# this is the shipping path (issue #219): the packages in #221/#222 are built
# from what this script produces, and a user installs them without a compiler.
# 62 translation units, against 711 files for the full php-src build.
#
# WHAT IT IS NOT. It cannot speak for `pool.type = fastcgi-ng` or
# `pool.type = http` -- they need zend_signal_use_persistent_handlers(), added
# by patches/0006 inside Zend/, which is the distribution's file and not ours.
# Nor for fibers/async (patches/0007, 0008, likewise inside libphp), nor for
# static-musl (no distribution ships a static libphp). The binary produced here
# REFUSES those pool types at startup rather than running them on upstream
# behaviour under their name -- issue #214, asserted below on the binary this
# run produced.
#
# Measured on 2026-09-11, Ubuntu 26.04, php8.5-dev 8.5.4: 14 s wall clock for
# 58 sources out of config.m4 plus 5, and the owned .phpt suite reports
# PASS=36 FAIL=0 SKIP=31 of 67 against this binary. The 31 skips are the tests
# whose pool needs one of the two types refused above; they ask the binary and
# skip rather than fail (issue #230), which is what makes this suite usable as
# a gate for the package.
#
# Usage: build/libphp-build.sh [php-src-tree] [outdir]
#   php-src-tree  a tree with our overlay applied (build/prepare.sh), default ./php-src
#   outdir        default ./out-libphp
#   PHP_CONFIG    php-config binary to build against; autodetected otherwise
set -eu

fail() {
  echo "libphp-build.sh: FAIL: $*" >&2
  exit 1
}

SRC=$(cd "${1:-./php-src}" 2>/dev/null && pwd) || fail "no php-src tree at ${1:-./php-src}"
OUT=${2:-./out-libphp}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)

[ -f "$SRC/sapi/fpmng/config.m4" ] || fail "$SRC has no sapi/fpmng: run build/prepare.sh first"

# --- the toolchain we build against -------------------------------------------
# php-config is version-namespaced on both distributions the spike measured
# (php-config8.5 on Ubuntu, php-config85 on Alpine), so try the specific names
# before the generic one -- a generic php-config may belong to another minor,
# and building against one minor's headers while linking another's .so is the
# exact hazard issue #220 guards at runtime.
if [ -z "${PHP_CONFIG:-}" ]; then
  for c in php-config8.5 php-config85 php-config; do
    command -v "$c" >/dev/null 2>&1 && { PHP_CONFIG=$c; break; }
  done
fi
[ -n "${PHP_CONFIG:-}" ] || fail "no php-config found; install php8.5-dev (Debian/Ubuntu) or php85-dev (Alpine)"
CORE_INC=$("$PHP_CONFIG" --includes)
PHP_VER=$("$PHP_CONFIG" --version)
INC_DIR=$("$PHP_CONFIG" --include-dir)
PHP_CONFIG_H="$INC_DIR/main/php_config.h"
[ -f "$PHP_CONFIG_H" ] || fail "$PHP_CONFIG points at $INC_DIR, which has no main/php_config.h"
echo "libphp-build.sh: $PHP_CONFIG -> PHP $PHP_VER, headers in $INC_DIR"

# ZTS (issue #213). sapi/fpmng/fpm/fpm_libphp_compat.c substitutes the
# unexported zend_signal_init() with zend_signal_startup(), which is idempotent
# for a freshly forked NTS child and is NOT under ZTS: there it reaches
# ts_allocate_fast_id() a second time and the engine ends up with two copies of
# the signal globals. Refused here rather than compiled: the miscompile would
# link, start and serve, and only lose signals.
if grep -q '^#define ZTS 1' "$PHP_CONFIG_H"; then
  fail "$PHP_CONFIG is a ZTS (thread-safe) build of PHP. This path substitutes zend_signal_init()
  with zend_signal_startup() (sapi/fpmng/fpm/fpm_libphp_compat.c), which is only
  equivalent under NTS. Install the NTS embed package, or build from source."
fi

# --- the defines a configure run would have made -------------------------------
# There is no configure on this path, so every AC_DEFINE has to be supplied by
# hand -- and a missing one is not a build error, it is a silently smaller
# binary. The spike's first attempt scored 26 PASS / 22 FAIL / 11 SKIP purely
# because HAVE_FPM_HTTP and HAVE_FPM_HTTP_TLS were absent, which compiled the
# gateway and TLS out without one warning.
#
# So the list below is checked against the config.m4 files rather than trusted:
# every AC_DEFINE they make must appear in exactly one of the three lists, and
# an unclassified name stops the build naming itself. That is what keeps this
# script from drifting the next time sapi/fpmng/config.m4 learns a feature.

# Supplied, because the FPM sources need them and the distribution's
# php_config.h describes the distribution's build host, not ours.
SUPPLIED='-DHAVE_CONFIG_H
-DHAVE_EPOLL=1
-DHAVE_SELECT=1
-DHAVE_BUILTIN_ATOMIC=1
-DHAVE_LQ_TCP_INFO=1
-DHAVE_TIMES=1
-DHAVE_CLEARENV=1
-DHAVE_CLOCK_GETTIME=1
-DHAVE_FPM_HTTP=1
-DHAVE_FPM_HTTP_TLS=1
-DFPMNG_LIBPHP_BUILD=1
-DPROC_MEM_FILE="mem"'

# Deliberately off, with the reason. These are not oversights, and anyone
# tempted to add one should read the reason first.
off_reason() {
  case "$1" in
  HAVE_CLOCK_GET_TIME)          echo "macOS clock_get_time(); this path is Linux-only" ;;
  HAVE_MACH_VM_READ)            echo "macOS trace backend; we use the pread one" ;;
  HAVE_PTRACE)                  echo "we compile fpm_trace_pread.c, not the ptrace backend" ;;
  HAVE_KQUEUE)                  echo "BSD event backend; HAVE_EPOLL is supplied instead" ;;
  HAVE_LQ_TCP_CONNECTION_INFO)  echo "macOS listen-queue probe" ;;
  HAVE_LQ_SO_LISTENQ)           echo "BSD listen-queue probe" ;;
  HAVE_SYSTEMD)                 echo "would add a libsystemd link the packages do not want" ;;
  HAVE_APPARMOR|HAVE_SELINUX)   echo "would add a link-time dependency; not offered by the packages" ;;
  HAVE_FPMNG_FIBER|HAVE_FPMNG_FIBER_TLS|HAVE_FPMNG_ASYNC)
                                echo "patches/0007 and 0008 apply inside libphp, which is the distribution's file" ;;
  HAVE_FPMNG_PERSISTENT_SIGNALS)
                                echo "patches/0006 applies inside Zend/, which is the distribution's file; leaving it unset is what makes pool.type = fastcgi-ng and http refuse to start here instead of running on a no-op (issue #214)" ;;
  *) return 1 ;;
  esac
}

# Taken from the distribution's php_config.h when it is there. HAVE_FPM_ACL is
# the one that bites: Alpine's header carries it and Ubuntu's does not, and on
# Alpine that means the build needs -lacl whether or not we want FPM's ACL
# support. It describes their build host, so it is discovered, not decided.
FROM_DISTRO='HAVE_FPM_ACL'

defines_in_config_m4() {
  for m in "$SRC/sapi/fpmng/config.m4" "$SRC/sapi/fpm/config.m4"; do
    [ -f "$m" ] && sed -n 's/.*AC_DEFINE\(_UNQUOTED\)\{0,1\}(\[\{0,1\}\([A-Z_0-9]\{1,\}\).*/\2/p' "$m"
  done | sort -u
}

unclassified=
for name in $(defines_in_config_m4); do
  case "$SUPPLIED" in *"-D$name"*) continue ;; esac
  case " $FROM_DISTRO " in *" $name "*) continue ;; esac
  off_reason "$name" >/dev/null && continue
  unclassified="$unclassified $name"
done
[ -z "$unclassified" ] || fail "config.m4 defines these, and this script does not say what to do with them:$unclassified.
  Add each to SUPPLIED, to FROM_DISTRO, or to off_reason() with a reason. Do not
  guess: HAVE_FPM_HTTP and HAVE_FPM_HTTP_TLS once went missing here and cost 22
  test failures with no build warning."

# HAVE_FPM_ACL: discovered from their header, and it decides a link flag.
ACL_LIBS=
if grep -q '^#define HAVE_FPM_ACL' "$PHP_CONFIG_H"; then
  ACL_LIBS=-lacl
  echo "libphp-build.sh: distribution php_config.h has HAVE_FPM_ACL, linking $ACL_LIBS"
fi

# --- include path --------------------------------------------------------------
# Our patched main/fastcgi.h has to shadow the one php8.5-dev ships, which is
# the unpatched upstream copy. One directory holding only that file: a blanket
# -I$SRC/main would shadow php.h and every other main header with our tree's
# copies, which belong to a different build than the .so we link against.
rm -rf "$OUT/fcgi-inc"
mkdir -p "$OUT/fcgi-inc" "$OUT/obj"
cp "$SRC/main/fastcgi.h" "$OUT/fcgi-inc/fastcgi.h"

# ext/fpmng_metrics includes "config.h" under HAVE_CONFIG_H -- the autoconf
# header a php-src build generates, which does not exist on this path. The
# distribution's php_config.h is its equivalent, and HAVE_CONFIG_H has to stay
# set because the rest of php-src's headers key off it.
mkdir -p "$OUT/compat"
cat > "$OUT/compat/config.h" <<'SHIM'
/* Generated by build/libphp-build.sh. There is no configure run on the libphp
 * path, so the header php-src would have written does not exist; the
 * distribution's php_config.h from php8.5-dev is what describes this build. */
#include "php_config.h"
SHIM

INC="-I$OUT/fcgi-inc $CORE_INC -I$SRC/sapi/fpmng -I$SRC/sapi/fpmng/fpm -I$SRC/ext/fpmng_metrics -I$REPO/build/libphp -I$OUT/compat"
# -D_GNU_SOURCE is required, not stylistic: Zend/zend_operators.h:235 uses
# memrchr(), which glibc hides behind it.
# EXTRA_CFLAGS exists for the version-guard demonstration in
# build/libphp/libphp_abi_check.c, which cannot be triggered with packages that
# exist; see the comment there.
CFLAGS="-D_GNU_SOURCE -O2 -g -fno-strict-aliasing -Wno-deprecated-declarations ${EXTRA_CFLAGS:-}"
DEFS=$(echo "$SUPPLIED" | tr '\n' ' ')

# --- sources -------------------------------------------------------------------
# Read out of config.m4 rather than copied into a second list here. A copy
# drifts the first time a file is added to the SAPI, and the symptom would be a
# link error in the best case and a missing feature in the worst.
sources() {
  sed -n '/PHP_FPMNG_FILES="/,/^[[:space:]]*"[[:space:]]*$/p' "$SRC/sapi/fpmng/config.m4" |
    grep -oE 'fpm/[A-Za-z0-9_/]+\.c' | sort -u
}
src_count=$(sources | wc -l)
[ "$src_count" -gt 20 ] || fail "only $src_count sources parsed out of sapi/fpmng/config.m4; the PHP_FPMNG_FILES block moved"

: > "$OUT/compile.log"
OBJS=""
compile() {
  o="$OUT/obj/$(echo "${2:-$(basename "$1")}" | tr /. __).o"
  # shellcheck disable=SC2086
  ${CC:-gcc} $CFLAGS $DEFS $INC -c "$1" -o "$o" >>"$OUT/compile.log" 2>&1 ||
    { echo "=== COMPILE FAILED: $1 ==="; tail -20 "$OUT/compile.log"; exit 1; }
  OBJS="$OBJS $o"
}
for f in $(sources); do compile "$SRC/sapi/fpmng/$f" "$f"; done
# Not in PHP_FPMNG_FILES: config.m4 adds the trace backend conditionally, our
# patched fastcgi.c belongs to main/, and the ABI guard is a property of this
# build rather than of the SAPI. The zend_signal_init() stand-in is NOT here:
# it lives in sapi/fpmng/fpm/fpm_libphp_compat.c and arrives through the source
# list above, gated on -DFPMNG_LIBPHP_BUILD (issue #213).
#
# ext/fpmng_metrics is compiled here but NOT registered here: on this path there
# is no configure to put it in main/internal_functions.c, and the static module
# list belongs to the distribution's libphp. The same FPMNG_LIBPHP_BUILD file
# registers it at runtime from fpm_init(), so the userland fpm_metric_*()
# functions exist in a worker on both builds (issue #216). What differs is
# `php-fpm-ng -m` and `-i`, neither of which ever reaches fpm_init().
for f in "$SRC/sapi/fpmng/fpm/fpm_trace.c" "$SRC/sapi/fpmng/fpm/fpm_trace_pread.c" \
         "$SRC/main/fastcgi.c" "$SRC/ext/fpmng_metrics/fpmng_metrics.c" \
         "$REPO/build/libphp/libphp_abi_check.c"; do
  compile "$f"
done

# --- link ----------------------------------------------------------------------
# Where libphp lives and what it is called is a distribution decision, so it is
# searched for rather than assumed: Ubuntu ships /usr/lib/libphp8.5.so, Alpine
# ships /usr/lib/php85/libphp.so. Hard-coding either one produces "cannot find
# -lphp8.5" on the other.
PHP_MM=$(echo "$PHP_VER" | cut -d. -f1,2)
LIBPHP_DIR= LIBPHP_NAME=
for dir in "$(dirname "$("$PHP_CONFIG" --extension-dir)")" "$("$PHP_CONFIG" --prefix)/lib" /usr/lib /usr/lib64; do
  for name in "php$PHP_MM" php; do
    if [ -e "$dir/lib$name.so" ]; then LIBPHP_DIR=$dir; LIBPHP_NAME=$name; break 2; fi
  done
done
[ -n "$LIBPHP_NAME" ] || fail "no libphp shared object found for $PHP_CONFIG; install the embed package (libphp${PHP_MM}-embed, or php$(echo "$PHP_MM" | tr -d .)-embed on Alpine)"
echo "libphp-build.sh: linking $LIBPHP_DIR/lib$LIBPHP_NAME.so"

# musl folds dl, rt and pthread into libc and ships no separate archives, so
# naming them is a link error there rather than a no-op. Probe instead of
# branching on the libc, which would be one more thing to get wrong per distro.
OPT_LIBS=
for l in dl rt pthread; do
  echo 'int main(void){return 0;}' | ${CC:-gcc} -x c - "-l$l" -o /dev/null 2>/dev/null &&
    OPT_LIBS="$OPT_LIBS -l$l"
done

BIN="$OUT/php-fpm-ng"
# shellcheck disable=SC2086
# -rpath: Alpine puts libphp.so in a version-namespaced directory that is not
# on the default search path, so without it the binary links and then cannot
# start. On Ubuntu the directory is already default and the flag is inert.
${CC:-gcc} -o "$BIN" $OBJS -L"$LIBPHP_DIR" -Wl,-rpath,"$LIBPHP_DIR" "-l$LIBPHP_NAME" $ACL_LIBS \
  -levent -levent_openssl -lssl -lcrypto -lm $OPT_LIBS -Wl,-E \
  >"$OUT/link.log" 2>&1 || {
  echo "=== LINK FAILED ==="
  grep "undefined reference" "$OUT/link.log" | sed 's/.*undefined reference to //' | sort -u | head -20
  exit 1
}

# --- assert the binary, not the flags ------------------------------------------
# Same reasoning as build/static-full.sh (issue #77): the flags above are
# exactly what was wrong when this went wrong, so a check that reads them would
# have passed. HAVE_FPM_HTTP and HAVE_FPM_HTTP_TLS compile whole files out, and
# the only honest evidence that they were set is a symbol from those files.
assert_symbol() {
  nm --defined-only "$BIN" 2>/dev/null | grep -qw -- "$1" ||
    fail "the binary has no '$1': $2"
}
assert_symbol fpm_http_init_pool "HAVE_FPM_HTTP was not set, so the http gateway is compiled out"
assert_symbol fpm_http_tls_validate "HAVE_FPM_HTTP_TLS was not set, so TLS is compiled out"
assert_symbol zif_fpmng_worker_respond "pool.executor = worker cannot answer a request without the fpmng_worker_* builtins"

# --- assert the refusal, not the define ----------------------------------------
# Issue #214. HAVE_FPMNG_PERSISTENT_SIGNALS being absent from the compile line
# is not evidence of anything: what matters is that a pool whose behaviour
# depends on patches/0006 does not start here. -t runs the same
# fpm_conf_post_process() a real start runs, so this exercises the production
# path and costs a few milliseconds.
# FPM refuses to run as root without a user/group to drop to, and the two
# distributions this build targets do not agree on what that pair is called
# (Ubuntu has no group "nobody"; Alpine does). Asked of the system rather than
# assumed, and left out entirely when this is not root, where FPM would only
# warn that it cannot honour it.
DROP_TO=
if [ "$(id -u)" = 0 ]; then
  DROP_TO="user = nobody
group = $(id -gn nobody)"
fi

conf_test() {
  cat > "$OUT/type-check.conf" <<EOT
[global]
error_log = /dev/stderr
[typecheck]
listen = 127.0.0.1:9
$DROP_TO
pm = static
pm.max_children = 1
pool.type = $1
${2:-}
EOT
  "$BIN" -n -t -y "$OUT/type-check.conf" 2>&1
}

# http-direct has directives of its own that its validate() requires, and that
# validate() runs after the check under test. They are here so that a failure
# below means what it says rather than "http-direct needs a chdir".
echo '<?php' > "$OUT/index.php"
HTTP_DIRECT_CONF="chdir = $OUT
http.front_controller = /index.php"
for t in fastcgi-ng http; do
  if out=$(conf_test "$t"); then
    fail "'pool.type = $t' was accepted by a binary that does not carry patches/0006; the pool would have run with upstream signal behaviour and said nothing"
  fi
  case "$out" in
  *"does not carry patches/0006"*) ;;
  *) fail "'pool.type = $t' was rejected, but not for the reason this build has: $out" ;;
  esac
done
for t in fastcgi http-direct; do
  [ "$t" = http-direct ] && extra=$HTTP_DIRECT_CONF || extra=
  if ! out=$(conf_test "$t" "$extra"); then
    fail "'pool.type = $t' is one of the two types this build exists to ship, and it does not even pass a configuration test: $out"
  fi
done
echo "libphp-build.sh: pool.type fastcgi and http-direct accepted, fastcgi-ng and http refused (issue #214)"

ldd "$BIN" | grep -qi "libphp" || fail "the binary does not link a distribution libphp; this is not the build this script is for"
echo "libphp-build.sh: $(ldd "$BIN" | grep -i libphp | tr -s ' ')"
"$BIN" -v | head -1
echo "libphp-build.sh: PASS ($src_count sources from config.m4 + $(echo $OBJS | wc -w | tr -d " ") objects total, -> $BIN)"
