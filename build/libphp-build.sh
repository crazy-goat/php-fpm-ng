#!/bin/sh
# Build php-fpm-ng from this repository and a distribution's PHP SDK: its
# headers and its prebuilt libphp. No php-src tree, no engine build
# (issues #212, #422; the spike was #198, docs/spike-libphp-link-report.md).
#
# WHAT THIS BUILD IS. It is the supported build of main (contract in #419:
# PHP 8.5 NTS, Linux with glibc or musl, dynamic linking, SDKs from the
# official Debian/Ubuntu/Alpine repositories). The packages are built from
# what this script produces (#221/#222), and a user installs them without a
# compiler. The inputs are:
#
#   - this repository's sapi/fpmng/fpm/, ext/fpmng_metrics/, build/libphp/;
#   - third_party/php-src/, the bounded upstream subset (issue #421): the FPM
#     files we do not override and the patched main/fastcgi.c/.h;
#   - the SDK: `php-config --includes` and libphp, plus the PHP CLI of the
#     same installation for the payload packer;
#   - libevent, and OpenSSL for TLS builds.
#
# Nothing is read from an upstream config.m4 or Makefile. The source list is
# whatever build/standalone-tree.sh assembles; every preprocessor feature
# macro those sources test is classified below, and the host capabilities the
# build asserts are compile-probed rather than assumed. The build ends with an
# audit of the compiler's own dependency output (build/audit-compile-deps.sh),
# so "no php-src was read" is checked, not claimed. <outdir>/commands.log
# records every compile and link command verbatim.
#
# Usage: build/libphp-build.sh [outdir]
#   outdir             default ./out-libphp
#   PHP_CONFIG         php-config binary to build against; autodetected otherwise
#   FPMNG_TLS          1 to build TLS termination in; default 0 (issue #280)
#   FPMNG_ACME         1 to build ACME issuance in; default 0, needs FPMNG_TLS=1 (issue #281)
#   FPMNG_DEBUG_CLOCK  1 to build the test-suite clock in; default 0, NEVER for a
#                      shipped binary (issue #396; the packages assert it is off)
#   CC, EXTRA_CFLAGS   compiler and extra flags
set -eu

fail() {
  echo "libphp-build.sh: FAIL: $*" >&2
  exit 1
}

REPO=$(cd "$(dirname "$0")/.." && pwd)

# The php-src tree argument this script used to take (issue #422). A caller
# still passing one would otherwise get its outdir in the wrong place.
if [ $# -gt 1 ]; then
  fail "this build takes no php-src tree any more (issue #422): usage: build/libphp-build.sh [outdir]"
fi
OUT=${1:-./out-libphp}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

# --- the platform -------------------------------------------------------------
# Linux only (#419). The supplied capabilities below are Linux ones (epoll,
# /proc/<pid>/mem, TCP_INFO); on another kernel the probes would fail one by
# one with messages about a header, which says nothing about why.
OS=$(uname -s)
[ "$OS" = Linux ] || fail "this build supports Linux only (glibc or musl), and this is $OS.
  macOS is a from-source development platform: build php-src with
  build/prepare.sh as described in docs/install.md."

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
command -v "$PHP_CONFIG" >/dev/null 2>&1 || fail "PHP_CONFIG=$PHP_CONFIG is not an executable"
CORE_INC=$("$PHP_CONFIG" --includes)
PHP_VER=$("$PHP_CONFIG" --version)
INC_DIR=$("$PHP_CONFIG" --include-dir)
PHP_MM=$(echo "$PHP_VER" | cut -d. -f1,2)
PHP_CONFIG_H="$INC_DIR/main/php_config.h"
[ -f "$PHP_CONFIG_H" ] || fail "$PHP_CONFIG points at $INC_DIR, which has no main/php_config.h"
echo "libphp-build.sh: $PHP_CONFIG -> PHP $PHP_VER, headers in $INC_DIR"

# PHP 8.5 only (#419). 8.4 lacks php_glob.h, which fpm_conf.c includes, and an
# 8.6 SDK is a different set of struct layouts nobody has tested this tree
# against. Either would fail somewhere in the middle of the compile, or worse
# not fail; this says it up front.
[ "$PHP_MM" = 8.5 ] || fail "$PHP_CONFIG is PHP $PHP_VER, and this tree supports PHP 8.5 only (issue #419).
  Install php8.5-dev and libphp8.5-embed (Debian/Ubuntu) or php85-dev and
  php85-embed (Alpine), or point PHP_CONFIG at an 8.5 php-config."

# ZTS (issue #213). sapi/fpmng/fpm/fpm_libphp_compat.c substitutes the
# unexported zend_signal_init() with zend_signal_startup(), which is idempotent
# for a freshly forked NTS child and is NOT under ZTS: there it reaches
# ts_allocate_fast_id() a second time and the engine ends up with two copies of
# the signal globals. Refused here rather than compiled: the miscompile would
# link, start and serve, and only lose signals. ZTS is out of the contract
# permanently (#419).
if grep -q '^#define ZTS 1' "$PHP_CONFIG_H"; then
  fail "$PHP_CONFIG is a ZTS (thread-safe) build of PHP. This path substitutes zend_signal_init()
  with zend_signal_startup() (sapi/fpmng/fpm/fpm_libphp_compat.c), which is only
  equivalent under NTS, and ZTS is outside the supported contract (issue #419).
  Install the NTS embed package."
fi

CC=${CC:-gcc}
command -v "$CC" >/dev/null 2>&1 || fail "no C compiler ($CC); install build-essential (Debian/Ubuntu) or build-base (Alpine)"

# --- options --------------------------------------------------------------------
toggle() {
  case "$2" in
  0|1) ;;
  *) fail "$1 must be 0 or 1, not '$2'" ;;
  esac
}
FPMNG_TLS=${FPMNG_TLS:-0}
FPMNG_ACME=${FPMNG_ACME:-0}
FPMNG_DEBUG_CLOCK=${FPMNG_DEBUG_CLOCK:-0}
toggle FPMNG_TLS "$FPMNG_TLS"
toggle FPMNG_ACME "$FPMNG_ACME"
toggle FPMNG_DEBUG_CLOCK "$FPMNG_DEBUG_CLOCK"
# ACME needs TLS for the reason config.m4 gives -- a certificate this binary
# could not serve -- and asking for one without the other is refused here
# exactly as `configure` refuses it.
[ "$FPMNG_ACME" = 0 ] || [ "$FPMNG_TLS" = 1 ] ||
  fail "FPMNG_ACME=1 needs FPMNG_TLS=1: ACME obtains a certificate, and a binary built without TLS termination has nothing to serve it with (issue #281)"

# --- the sources ----------------------------------------------------------------
"$REPO/build/vendor-php-src.sh" check >/dev/null ||
  fail "third_party/php-src does not match its manifest; see build/vendor-php-src.sh check"
TREE=$OUT/src
"$REPO/build/standalone-tree.sh" "$TREE"
rm -rf "$OUT/obj" "$OUT/dep" "$OUT/compat" "$OUT/probe"
mkdir -p "$OUT/obj" "$OUT/dep" "$OUT/compat" "$OUT/probe"

# --- host capabilities, probed ---------------------------------------------------
# There is no configure on this path, and the distribution's php_config.h
# describes the distribution's build of PHP, not ours: Ubuntu's leaves
# HAVE_EPOLL and HAVE_ACCEPT4 undefined because its embed build had no FPM and
# no ext/sockets. So each capability the FPM sources test is compiled and
# linked here, with the same test program upstream's configure uses, and a
# missing one stops the build by name.
PROBE_CFLAGS="-D_GNU_SOURCE ${EXTRA_CFLAGS:-}"
probe() {
  name=$1
  shift
  printf '%s\n' "$@" > "$OUT/probe/$name.c"
  # shellcheck disable=SC2086
  $CC $PROBE_CFLAGS "$OUT/probe/$name.c" -o "$OUT/probe/$name" >"$OUT/probe/$name.log" 2>&1 ||
    fail "the host lacks $name, which this Linux build relies on; the probe and its compiler output are in $OUT/probe/$name.c and .log"
}
probe HAVE_EPOLL '#include <sys/epoll.h>' 'int main(void) { struct epoll_event e; (void) e; return epoll_create(1) < 0; }'
probe HAVE_SELECT '#include <sys/select.h>' 'int main(void) { fd_set s; FD_ZERO(&s); return select(0, &s, 0, 0, 0); }'
probe HAVE_BUILTIN_ATOMIC 'int main(void) { int v = 1; return (__sync_bool_compare_and_swap(&v, 1, 2) && __sync_add_and_fetch(&v, 1)) ? 1 : 0; }'
probe HAVE_LQ_TCP_INFO '#include <netinet/tcp.h>' 'int main(void) { struct tcp_info ti; int x = TCP_INFO; (void) ti; return x < 0; }'
probe HAVE_TIMES '#include <sys/times.h>' 'int main(void) { struct tms t; return times(&t) == (clock_t) -1; }'
probe HAVE_CLEARENV '#include <stdlib.h>' 'int main(void) { return clearenv(); }'
probe HAVE_CLOCK_GETTIME '#include <time.h>' 'int main(void) { struct timespec ts; return clock_gettime(CLOCK_MONOTONIC, &ts); }'
# patches/0003: accept4(SOCK_CLOEXEC) saves two fcntl() calls per accepted
# connection. Ubuntu's php_config.h does not define it, so before #422 the
# Ubuntu packages silently compiled the accept()+fcntl() fallback.
probe HAVE_ACCEPT4 '#include <sys/socket.h>' 'int main(void) { return accept4(-1, 0, 0, SOCK_CLOEXEC) == 0; }'
# PROC_MEM_FILE: the slowlog reads a stuck child's stack through
# /proc/<pid>/mem with pread() (fpm_trace_pread.c). Upstream's configure runs
# this program; so does this build, because the answer is a property of the
# kernel the build runs on and the packages run on the same kind.
probe PROC_MEM_FILE '#define _FILE_OFFSET_BITS 64' '#include <stdint.h>' '#include <stdio.h>' '#include <unistd.h>' '#include <fcntl.h>' \
  'int main(void) { long v1 = (unsigned int) -1, v2 = 0; char buf[128]; int fd;' \
  '  snprintf(buf, sizeof(buf), "/proc/%d/mem", (int) getpid()); fd = open(buf, O_RDONLY);' \
  '  if (fd < 0) return 1; if (pread(fd, &v2, sizeof(long), (uintptr_t) &v1) != sizeof(long)) return 1;' \
  '  close(fd); return v1 != v2; }'
"$OUT/probe/PROC_MEM_FILE" ||
  fail "pread() on /proc/<pid>/mem does not work here, which the slowlog backend (fpm_trace_pread.c) relies on"
echo "libphp-build.sh: probed epoll, select, __sync atomics, TCP_INFO, times, clearenv, clock_gettime, accept4, /proc/<pid>/mem"

# --- the defines, classified -----------------------------------------------------
# A missing define is not a build error, it is a silently smaller binary. The
# spike's first attempt scored 26 PASS / 22 FAIL / 11 SKIP purely because
# HAVE_FPM_HTTP and HAVE_FPM_HTTP_TLS were absent, and HAVE_ACCEPT4 was
# missing from this build for months. So every feature macro the compiled
# sources test in a preprocessor conditional must be accounted for: supplied
# here, defined by the SDK or by the sources themselves, or deliberately off
# with a reason. An unclassified name stops the build.
SUPPLIED='-DHAVE_CONFIG_H
-DHAVE_EPOLL=1
-DHAVE_SELECT=1
-DHAVE_BUILTIN_ATOMIC=1
-DHAVE_LQ_TCP_INFO=1
-DHAVE_TIMES=1
-DHAVE_CLEARENV=1
-DHAVE_CLOCK_GETTIME=1
-DHAVE_ACCEPT4=1
-DHAVE_FPM_HTTP=1
-DFPMNG_LIBPHP_BUILD=1
-DPROC_MEM_FILE="mem"'

# TLS termination is opt-in here for the same reason it is opt-in in
# configure (--enable-fpmng-tls, issue #280): this is the path the shipped
# packages are built from, so a flag that did not reach it would govern
# nothing that ships. Off means three things at once, and all three are
# asserted on the produced binary below: HAVE_FPM_HTTP_TLS unset, the
# fpm_tls_*.c sources not compiled, and no OpenSSL on the link line.
if [ "$FPMNG_TLS" = 1 ]; then
  SUPPLIED="$SUPPLIED
-DHAVE_FPM_HTTP_TLS=1"
  echo "libphp-build.sh: FPMNG_TLS=1, building with TLS termination"
else
  echo "libphp-build.sh: FPMNG_TLS=0, building without TLS termination (set FPMNG_TLS=1 for it)"
fi
if [ "$FPMNG_ACME" = 1 ]; then
  SUPPLIED="$SUPPLIED
-DHAVE_FPMNG_ACME=1"
  echo "libphp-build.sh: FPMNG_ACME=1, building with ACME issuance"
else
  echo "libphp-build.sh: FPMNG_ACME=0, building without ACME issuance (set FPMNG_ACME=1 for it)"
fi
if [ "$FPMNG_DEBUG_CLOCK" = 1 ]; then
  SUPPLIED="$SUPPLIED
-DHAVE_FPMNG_DEBUG_CLOCK=1"
  echo "libphp-build.sh: FPMNG_DEBUG_CLOCK=1: this binary's clock can be made to run faster than real time by an environment variable. It is for the test suite, never for a server or a package."
fi

# Deliberately off, with the reason. These are not oversights, and anyone
# tempted to add one should read the reason first.
off_reason() {
  case "$1" in
  HAVE_CLOCK_GET_TIME)          echo "macOS clock_get_time(); this path is Linux-only" ;;
  HAVE_MACH_VM_READ)            echo "macOS trace backend; we use the pread one" ;;
  HAVE_PTRACE)                  echo "we compile fpm_trace_pread.c, not the ptrace backend" ;;
  HAVE_KQUEUE)                  echo "BSD event backend; HAVE_EPOLL is supplied instead" ;;
  HAVE_PORT_CREATE)             echo "Solaris event ports; HAVE_EPOLL is supplied instead" ;;
  HAVE_LQ_TCP_CONNECTION_INFO)  echo "macOS listen-queue probe" ;;
  HAVE_LQ_SO_LISTENQ)           echo "BSD listen-queue probe" ;;
  HAVE_SETPROCTITLE|HAVE_SETPROCTITLE_FAST) echo "BSD libc functions; on Linux fpm_env.c rewrites argv itself" ;;
  HAVE_SETPFLAGS)               echo "Solaris privilege flags" ;;
  HAVE_PROCCTL)                 echo "FreeBSD procctl(); Linux uses prctl()" ;;
  HAVE_STRUCT_SOCKADDR_UN_SUN_LEN) echo "BSD sockaddr_un field; Linux has none" ;;
  HAVE_SYSTEMD)                 echo "would add a libsystemd link the packages do not want" ;;
  HAVE_APPARMOR|HAVE_SELINUX)   echo "would add a link-time dependency; not offered by the packages" ;;
  HAVE_FPM_HTTP_TLS)            echo "TLS termination is opt-in (FPMNG_TLS=1 here, --enable-fpmng-tls in configure); issue #280" ;;
  HAVE_FPMNG_ACME)              echo "ACME issuance is opt-in (FPMNG_ACME=1 here, --enable-fpmng-acme in configure); issue #281" ;;
  HAVE_FPMNG_DEBUG_CLOCK)       echo "a clock an environment variable can make run faster than real time; FPMNG_DEBUG_CLOCK=1 is for the test suite only and the packages assert it is off (issue #396)" ;;
  USE_LOCKING)                  echo "fastcgi.c's accept() lock for platforms without a thread-safe accept(); never on Linux" ;;
  PHP_FPM_ZLOG_TRACE)           echo "scoreboard debug tracing, a developer switch upstream never enables" ;;
  FPMNG_BUILT_PHP_VERSION|FPMNG_BUILT_PHP_VERSION_ID) echo "test seam of build/libphp/libphp_abi_check.c, set only through EXTRA_CFLAGS" ;;
  *) return 1 ;;
  esac
}
# Off by our choice but present in a distribution's php_config.h, and harmless
# there. Alpine's header defines HAVE_PTRACE; in these sources it only feeds
# fpm_config.h's HAVE_FPM_TRACE, which PROC_MEM_FILE turns on anyway. Any
# other off-list name in the SDK header stops the build: it would switch on
# code the list above says must stay off.
SDK_TOLERATED='HAVE_PTRACE'

# Taken from the distribution's php_config.h when it is there. HAVE_FPM_ACL is
# the one that bites: Alpine's header carries it and Ubuntu's does not, and on
# Alpine that means the build needs -lacl whether or not we want FPM's ACL
# support. It describes their build host, so it is discovered, not decided.
FROM_DISTRO='HAVE_FPM_ACL HAVE_SYS_ACL_H'

scan_dirs="$TREE $REPO/ext/fpmng_metrics $REPO/build/libphp"
# Identifiers in #if/#ifdef/#ifndef/#elif lines with a feature-macro prefix.
# Splitting on non-identifier characters, rather than grep -o with \b, keeps
# this working on busybox and does not pick up ZEND_HAVE_X as HAVE_X.
# shellcheck disable=SC2086
referenced=$(find $scan_dirs -name '*.[ch]' -exec grep -hE '^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef|elif)' {} + |
  tr -c 'A-Za-z0-9_' '\n' | grep -E '^(HAVE|USE|FPMNG|PHP_FPM)_[A-Z0-9_]+$' | LC_ALL=C sort -u)
# Every name some header in the SDK or in the sources #defines. The SDK's
# php_config.h writes the ones it does not have as "/* #undef X */", which
# this does not match.
# shellcheck disable=SC2086
defined=$(find "$INC_DIR" $scan_dirs -name '*.h' -exec grep -hE '^[[:space:]]*#[[:space:]]*define[[:space:]]+[A-Za-z_]' {} + |
  sed 's/^[[:space:]]*#[[:space:]]*define[[:space:]]*\([A-Za-z0-9_]*\).*/\1/' | LC_ALL=C sort -u)

unclassified=
sdk_conflict=
for name in $referenced; do
  case "$SUPPLIED" in *"-D$name="*|*"-D$name
"*) continue ;; esac
  [ "$name" = HAVE_CONFIG_H ] && continue
  case " $FROM_DISTRO " in *" $name "*) continue ;; esac
  if off_reason "$name" >/dev/null; then
    if grep -qE "^#define $name([[:space:]]|$)" "$PHP_CONFIG_H" 2>/dev/null; then
      case " $SDK_TOLERATED " in *" $name "*) ;; *) sdk_conflict="$sdk_conflict $name" ;; esac
    fi
    continue
  fi
  echo "$defined" | grep -qx "$name" && continue
  unclassified="$unclassified $name"
done
[ -z "$unclassified" ] || fail "the sources test these macros, and this script does not say what to do with them:$unclassified.
  Add each to SUPPLIED (with a probe if it is a host capability), to
  FROM_DISTRO, or to off_reason() with a reason. Do not guess: HAVE_FPM_HTTP
  and HAVE_FPM_HTTP_TLS once went missing here and cost 22 test failures
  with no build warning."
[ -z "$sdk_conflict" ] || fail "$PHP_CONFIG_H defines$sdk_conflict, which this build keeps off (see off_reason() in this script); building against it would switch that code on"
echo "libphp-build.sh: $(echo "$referenced" | wc -l | tr -d ' ') feature macros tested by the sources, all classified"

# HAVE_FPM_ACL: discovered from their header, and it decides a link flag.
ACL_LIBS=
if grep -q '^#define HAVE_FPM_ACL' "$PHP_CONFIG_H"; then
  ACL_LIBS=-lacl
  echo "libphp-build.sh: distribution php_config.h has HAVE_FPM_ACL, linking $ACL_LIBS"
fi

# --- include path --------------------------------------------------------------
# $TREE/main first, and it holds only fastcgi.c/.h: our patched fastcgi.h
# has to shadow the unpatched one php8.5-dev ships, and nothing else in main/
# may shadow the SDK's (build/standalone-tree.sh).
#
# ext/fpmng_metrics includes "config.h" under HAVE_CONFIG_H -- the autoconf
# header a php-src build generates, which does not exist on this path. The
# distribution's php_config.h is its equivalent, and HAVE_CONFIG_H has to stay
# set because the rest of php-src's headers key off it.
cat > "$OUT/compat/config.h" <<'SHIM'
/* Generated by build/libphp-build.sh. There is no configure run on the libphp
 * path, so the header php-src would have written does not exist; the
 * distribution's php_config.h from php8.5-dev is what describes this build. */
#include "php_config.h"
SHIM

INC="-I$TREE/main $CORE_INC -I$TREE/sapi/fpmng -I$TREE/sapi/fpmng/fpm -I$REPO/ext/fpmng_metrics -I$REPO/build/libphp -I$OUT/compat"
# -D_GNU_SOURCE is required, not stylistic: Zend/zend_operators.h:235 uses
# memrchr(), which glibc hides behind it.
# EXTRA_CFLAGS exists for the version-guard demonstration in
# build/libphp/libphp_abi_check.c, which cannot be triggered with packages that
# exist; see the comment there.
CFLAGS="-D_GNU_SOURCE -O2 -g -fno-strict-aliasing -Wno-deprecated-declarations ${EXTRA_CFLAGS:-}"
DEFS=$(echo "$SUPPLIED" | tr '\n' ' ')

# --- compile -------------------------------------------------------------------
# Every .c the assembly holds, by the name-prefix rule build/prepare.sh uses
# for the optional groups: fpm_tls_*.c only with TLS, fpm_acme_*.c only with
# ACME. Found, not listed, so a new source file needs no edit here. The ones
# from outside sapi/: the patched fastcgi.c, the fpmng_metrics extension, and
# the ABI guard, which is a property of this build rather than of the SAPI.
#
# ext/fpmng_metrics is compiled here but NOT registered here: on this path there
# is no configure to put it in main/internal_functions.c, and the static module
# list belongs to the distribution's libphp. sapi/fpmng/fpm/fpm_libphp_compat.c
# registers it at runtime from fpm_init(), so the userland fpm_metric_*()
# functions exist in a worker on both builds (issue #216). What differs is
# `php-fpm-ng -m` and `-i`, neither of which ever reaches fpm_init() -- an
# accepted limitation (#419, docs/install.md).
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

{
  echo "# $(uname -srm); $($CC --version | head -n 1)"
  echo "# $PHP_CONFIG -> PHP $PHP_VER, $INC_DIR"
  echo "# FPMNG_TLS=$FPMNG_TLS FPMNG_ACME=$FPMNG_ACME FPMNG_DEBUG_CLOCK=$FPMNG_DEBUG_CLOCK EXTRA_CFLAGS=${EXTRA_CFLAGS:-}"
} > "$OUT/commands.log"
: > "$OUT/compile.log"
OBJS=""
n_src=0
for s in $(sources); do
  case "$s" in
    "$TREE"/*) rel=${s#"$TREE"/} ;;
    *) rel=${s#"$REPO"/} ;;
  esac
  base=$(echo "$rel" | tr /. __)
  cmd="$CC $CFLAGS $DEFS $INC -MD -MF $OUT/dep/$base.d -c $s -o $OUT/obj/$base.o"
  echo "$cmd" >> "$OUT/commands.log"
  # shellcheck disable=SC2086
  $cmd >>"$OUT/compile.log" 2>&1 ||
    { echo "=== COMPILE FAILED: $rel ==="; tail -n 20 "$OUT/compile.log"; exit 1; }
  OBJS="$OBJS $OUT/obj/$base.o"
  n_src=$((n_src + 1))
done

# What the compiler actually read: nothing outside the assembled tree, the
# repository, the SDK and the system; not the SDK's unpatched fastcgi.h; no
# main/ header from anywhere but the SDK; every vendored file used.
"$REPO/build/audit-compile-deps.sh" "$OUT/dep" "$INC_DIR" "$TREE" "$REPO/ext/fpmng_metrics" "$REPO/build/libphp" "$OUT/compat" ||
  fail "the compile consumed something it should not have (see above)"

# --- link ----------------------------------------------------------------------
# Where libphp lives and what it is called is a distribution decision, so it is
# searched for rather than assumed: Ubuntu ships /usr/lib/libphp8.5.so, Alpine
# ships /usr/lib/php85/libphp.so. Hard-coding either one produces "cannot find
# -lphp8.5" on the other.
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
  echo 'int main(void){return 0;}' | $CC -x c - "-l$l" -o /dev/null 2>/dev/null &&
    OPT_LIBS="$OPT_LIBS -l$l"
done

BIN="$OUT/php-fpm-ng"
# -rpath: Alpine puts libphp.so in a version-namespaced directory that is not
# on the default search path, so without it the binary links and then cannot
# start. On Ubuntu the directory is already default and the flag is inert.
TLS_LIBS=
[ "$FPMNG_TLS" = 1 ] && TLS_LIBS="-levent_openssl -lssl -lcrypto"
cmd="$CC -o $BIN $OBJS -L$LIBPHP_DIR -Wl,-rpath,$LIBPHP_DIR -l$LIBPHP_NAME $ACL_LIBS -levent $TLS_LIBS -lm $OPT_LIBS -Wl,-E"
echo "$cmd" >> "$OUT/commands.log"
# shellcheck disable=SC2086
$cmd >"$OUT/link.log" 2>&1 || {
  echo "=== LINK FAILED ==="
  grep "undefined reference" "$OUT/link.log" | sed 's/.*undefined reference to //' | sort -u | head -20
  exit 1
}

# --- embed the distribution payload (issue #171) -------------------------------
# After the link and before anything reads the binary, because this is an
# append: strip(1) would drop it (it is not an ELF section), so nothing may
# strip after this point. build/package-apk.sh already builds with `!strip`
# and build/package-deb.sh never strips.
#
# The packer is PHP, and the interpreter comes from the same installation as
# php-config rather than from PATH: this build already depends on that
# installation for libphp itself, and a `php` found on PATH could be a
# different version with a different set of extensions.
PHP_BIN=$("$PHP_CONFIG" --php-binary 2>/dev/null || true)
[ -n "$PHP_BIN" ] && [ -x "$PHP_BIN" ] || PHP_BIN=$(command -v php || true)
[ -n "$PHP_BIN" ] || fail "no PHP interpreter to run build/payload-pack.php; install the cli package next to php-config"
FPMNG_ACME="$FPMNG_ACME" "$REPO/build/embed-payload.sh" "$BIN" "$PHP_BIN" ||
  fail "the distribution payload could not be embedded"

# --- assert the binary, not the flags ------------------------------------------
# Issue #77's reasoning: the flags above are exactly what was wrong when this
# went wrong, so a check that reads them would have passed. HAVE_FPM_HTTP and
# HAVE_FPM_HTTP_TLS compile whole files out, and the only honest evidence that
# they were set is a symbol from those files.
assert_symbol() {
  nm --defined-only "$BIN" 2>/dev/null | grep -qw -- "$1" ||
    fail "the binary has no '$1': $2"
}
refute_symbol() {
  ! nm --defined-only "$BIN" 2>/dev/null | grep -qw -- "$1" ||
    fail "the binary has '$1': $2"
}
assert_symbol fpm_http_init_pool "HAVE_FPM_HTTP was not set, so the http gateway is compiled out"
assert_symbol zif_fpmng_worker_respond "pool.executor = worker cannot answer a request without the fpmng_worker_* builtins"
assert_symbol zif_fpm_metric_inc "the fpmng_metrics extension's userland functions are missing"
# The accept4() path of patches/0003 is inlined into fcgi_accept_request(), so
# its evidence is the call in the binary's dynamic symbol table.
nm -D --undefined-only "$BIN" 2>/dev/null | grep -qw accept4 ||
  fail "the binary does not call accept4(): HAVE_ACCEPT4 did not reach main/fastcgi.c (patches/0003)"

# TLS, in whichever direction was asked for. Both halves are asserted on the
# binary: the symbol says the sources were compiled, and the dynamic section
# says what the linker actually pulled in. Reading the flags instead would
# have passed in both of the ways this can go wrong -- a source group that
# leaked into the base list, and a -lssl left on the link line.
tls_linkage() {
  # The binary's OWN DT_NEEDED entries, not ldd: ldd prints the transitive
  # closure, and libphp.so itself links OpenSSL for ext/openssl. A default
  # build correctly shows libssl there through libphp and it means nothing
  # about this binary. readelf comes from binutils, which this path already
  # needs for nm.
  readelf -d "$BIN" 2>/dev/null | grep NEEDED |
    grep -cE 'libssl|libcrypto|libevent_openssl' || true
}
if [ "$FPMNG_TLS" = 1 ]; then
  assert_symbol fpm_tls_http_validate "FPMNG_TLS=1 was asked for, but the TLS sources were not compiled in"
  [ "$(tls_linkage)" -gt 0 ] || fail "FPMNG_TLS=1 was asked for, but the binary links no OpenSSL"
else
  refute_symbol fpm_tls_http_validate "a default build carries it: an fpm_tls_*.c source reached the base object list (issue #280)"
  [ "$(tls_linkage)" = 0 ] ||
    fail "a default build links OpenSSL: $(tls_linkage) OpenSSL entries in the dynamic section, and there should be none (issue #280)"
fi

# ACME, the same two directions, plus the payload. The payload matters as much
# as the symbol here: the ACME client is PHP, so a binary with no
# fpm_acme_challenge.c but the scripts still appended would carry the facility
# as data (issue #281). `list` prints one line per appended entry.
acme_payload_entries() {
  "$PHP_BIN" "$REPO/build/payload-pack.php" list --binary="$BIN" 2>/dev/null |
    grep -c 'kind=1' || true
}
if [ "$FPMNG_ACME" = 1 ]; then
  assert_symbol fpm_acme_challenge_init_main "FPMNG_ACME=1 was asked for, but the ACME sources were not compiled in"
  [ "$(acme_payload_entries)" -gt 0 ] ||
    fail "FPMNG_ACME=1 was asked for, but the binary carries no distribution payload, so fpmng-dist://acme/renew.php would not resolve"
else
  refute_symbol fpm_acme_challenge_init_main "a default build carries it: an fpm_acme_*.c source reached the base object list (issue #281)"
  [ "$(acme_payload_entries)" = 0 ] ||
    fail "a default build carries a distribution payload, and the only thing the payload holds is the ACME client (issue #281)"
fi

# The test-suite clock, both directions, because the packages assert its
# absence on this same symbol and a toggle that did not reach the sources
# would make the clock-scaled tests pass for the wrong reason (issue #396).
if [ "$FPMNG_DEBUG_CLOCK" = 1 ]; then
  assert_symbol fpm_debug_clock_now "FPMNG_DEBUG_CLOCK=1 was asked for, but fpm_debug_clock.c compiled to nothing"
else
  refute_symbol fpm_debug_clock_now "the test-suite clock is in a build that did not ask for it (issue #396)"
fi

# --- assert what is accepted, and that the retired name is refused -------------
# Issue #214, re-aimed by #388 and #420. No type keys on a build capability any
# more: #388 retired pool.type = http, the last type whose children needed
# patches/0006, and pool.type = gateway runs no PHP child at all; #420 then
# removed the patch and the build-support refusal outright. So this is a
# positive check -- every type this build exists to ship (fastcgi, gateway,
# http-direct) passes -t on this binary -- plus the one negative that still
# means something: the retired name is refused by name.
# -t runs the same fpm_conf_post_process() a real start runs.
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

# The gateway rejects pm/pm.max_children and needs at least one http.route[]
# with a target pool, so it cannot use conf_test()'s base. Its own minimal
# configuration, then. -t validates, it does not bind.
conf_test_gateway() {
  cat > "$OUT/type-check.conf" <<EOT
[global]
error_log = /dev/stderr
[typecheck]
pool.type = gateway
listen = 127.0.0.1:9
$DROP_TO
http.route[app] = /
[app]
listen = 127.0.0.1:8
$DROP_TO
pm = static
pm.max_children = 1
EOT
  "$BIN" -n -t -y "$OUT/type-check.conf" 2>&1
}

# http-direct has directives of its own that its validate() requires, and that
# validate() runs after the check under test. They are here so that a failure
# below means what it says rather than "http-direct needs a chdir".
echo '<?php' > "$OUT/index.php"
HTTP_DIRECT_CONF="chdir = $OUT
http.front_controller = /index.php"

for t in fastcgi http-direct; do
  [ "$t" = http-direct ] && extra=$HTTP_DIRECT_CONF || extra=
  if ! out=$(conf_test "$t" "$extra"); then
    fail "'pool.type = $t' is one of the types this build exists to ship, and it does not even pass a configuration test: $out"
  fi
done
if ! out=$(conf_test_gateway); then
  fail "pool.type = gateway is one of the types this build exists to ship, and it does not even pass a configuration test: $out"
fi
# pool.type = http is retired (issue #388): refused by name on every build.
# #420 removed the capability bit and its libphp guard, so only the refusal
# itself is asserted here.
if out=$(conf_test http); then
  fail "the retired 'pool.type = http' was accepted: $out"
fi
echo "libphp-build.sh: pool.type fastcgi, gateway and http-direct accepted, http retired (issues #214, #388, #420)"

ldd "$BIN" | grep -qi "libphp" || fail "the binary does not link a distribution libphp; this is not the build this script is for"
echo "libphp-build.sh: $(ldd "$BIN" | grep -i libphp | tr -s ' ')"
"$BIN" -v | head -n 1
echo "libphp-build.sh: PASS ($n_src translation units, commands in $OUT/commands.log -> $BIN)"
