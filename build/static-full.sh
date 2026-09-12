#!/bin/sh
set -eu

fail() {
  echo "static-full.sh: FAIL: $*" >&2
  exit 1
}

# ccache, and no --no-cache: /etc/apk/cache and /ccache are host directories
# kept between runs (issue #197). This job used to be the only compiling one
# in the matrix with no compiler cache at all, and it re-downloaded 25
# packages every time.
apk add ccache build-base autoconf bison re2c pkgconf linux-headers \
  libevent-dev libevent-static openssl-dev openssl-libs-static \
  zlib-dev zlib-static curl-dev curl-static libxml2-dev libxml2-static \
  sqlite-dev sqlite-static xz-static brotli-static nghttp2-static nghttp3-static \
  ngtcp2-static zstd-static libidn2-static libpsl-static libunistring-static \
  file \
  >/tmp/apk.log 2>&1 || { echo "=== MISSING PACKAGES ==="; tail -40 /tmp/apk.log; exit 1; }
export CCACHE_DIR="${CCACHE_DIR:-/ccache}"
export CC="ccache gcc" CXX="ccache g++"
cd /src
[ -f configure ] || ./buildconf --force
cd /build

# Issue #197: /build is a host directory kept between runs, so configure only
# runs when something it depends on changed. The key is the one ci-build-tree.sh
# already wrote for /src -- it covers the php-src pin, the patch stack, the
# overlay's file names and its config.m4 -- plus this script itself, which is
# where the configure line lives. Any mismatch wipes /build: a tree half-built
# from two configurations links without complaint.
key_file=/build/.fpmng-musl-key
key=$( { cat /src/.fpmng-ci-key /src/.fpmng-ci-epoch 2>/dev/null; cat /repo/build/static-full.sh; } | sha256sum | cut -d" " -f1)
reuse=no
if [ -f "$key_file" ] && [ "$(cat "$key_file")" = "$key" ] && [ -f /build/Makefile ] &&
   [ ! -f /build/.fpmng-musl-dirty ]; then
  reuse=yes
else
  # Empty it, do not remove it: /build is a bind mount, and unlinking a mount
  # point fails with EBUSY.
  find /build -mindepth 1 -delete
fi
echo "static-full.sh: /build reuse=$reuse key=$key"
# --enable-filter and --enable-ctype are not conveniences: they are hard
# `ext-*` requirements of the async client stack this binary exists to run.
# Under --disable-all both are off, and the failure is a runtime exception deep
# inside a library, not a configure or startup error. Measured in task 074
# against amphp/mysql 3.1.1 in examples/http-direct-worker-mysql:
#   filter: required directly by amphp/dns (`"ext-filter": "*"`) and by
#     league/uri-interfaces, which calls
#     filter_var($host, FILTER_VALIDATE_IP) in UriString.php:711, so
#     amphp/socket's connect() turned `tcp://mysql:3306` into
#     `Error: Invalid URI: tcp://mysql:3306`
#     (vendor/amphp/socket/src/Internal/functions.php:40) — no connection ever
#     opened.
#   ctype: amphp/dns depends on daverandom/libdns (`"ext-ctype": "*"`), which
#     parses the name to resolve, so hostname lookup is the next thing to fail
#     once filter is present.
# Both are tiny and dependency-free, and amphp/dns additionally requires
# ext-json and amphp/socket ext-openssl — those two were already in this build
# (verified with `strings`: json_encode and openssl_encrypt present,
# filter_var and ctype_digit absent).
if [ "$reuse" = no ]; then
  export PKG_CONFIG="pkg-config --static"
  LDFLAGS="-static-pie" /src/configure \
    --disable-all --enable-fpmng \
    --enable-opcache --enable-mbstring --disable-mbregex \
    --enable-sockets --enable-pcntl --enable-posix \
    --enable-filter --enable-ctype \
    --with-curl --with-openssl --with-zlib --enable-pdo --with-pdo-mysql=mysqlnd \
    --without-pear --disable-cgi --disable-phpdbg --disable-shared \
    --prefix=/usr/local > /out/configure-full.log 2>&1 || { echo "=== CONFIGURE FAILED ==="; tail -12 /out/configure-full.log; exit 1; }
  echo "$key" > "$key_file"
fi
grep -q 'S\["LIBEVENT_OPENSSL_LIBS"\]="[^"]*event_openssl' /build/config.status ||
  fail "libevent_openssl was not detected; TLS support would be missing"
# The marker survives a killed container (a cancelled workflow run), and the
# next run then throws /build away instead of trusting objects a SIGKILL
# truncated mid-write -- see build/ci-build-tree.sh for the same reasoning.
touch /build/.fpmng-musl-dirty
# `cli` as well as `fpmng`: the payload packer is PHP (see
# build/embed-payload.sh), and this container has no interpreter other than the
# one this tree can build. The objects are shared with fpmng, so it is a link,
# not a second build.
make -j"${JOBS:-8}" fpmng cli > /out/make-full.log 2>&1 || {
  echo "=== BUILD FAILED ==="
  grep -iE "error|undefined reference|cannot find -l" /out/make-full.log | sort -u | head -25
  exit 1
}
rm -f /build/.fpmng-musl-dirty
ccache --show-stats 2>/dev/null | head -5 || true

artifact=/build/sapi/fpmng/php-fpm-ng
[ -x "$artifact" ] || fail "expected artefact is missing: $artifact"
file_output=$(file "$artifact")
echo "$file_output"
echo "$file_output" | grep -q 'static-pie linked' ||
  fail "artefact is not static-pie linked"
# Issue #77: the configure line above is not evidence. It was already wrong
# once -- --enable-filter and --enable-ctype were missing, configure and the
# build were both happy, and the failure surfaced three layers away as
# `Error: Invalid URI: tcp://mysql:3306` from a userland library. So assert
# what is *in the artefact*, not what was asked for on the command line.
#
# `strings` rather than running the binary: the artefact is a musl static-pie
# php-fpm, and this script must also work when it is cross-inspected outside
# the container that produced it. An internal function's name is in the
# binary verbatim because the function table stores it.
#
# One entry per named consumer. An assertion nobody can explain gets deleted
# the first time it is inconvenient, so each line says who breaks without it.
# This is a change detector for the extension set, not a feature inventory:
# do not grow it beyond symbols something concrete in this repository needs.
#
# Demonstrated on 2026-09-11, two full Alpine builds of the same tree that
# differed only in the configure line. With --enable-filter present:
#   static-full.sh: extension-set assertions ok
#   static-full.sh: PASS
# With --enable-filter removed, configure and `make` still succeeded and the
# artefact was still a valid static-pie ELF -- which is exactly the failure
# mode this block exists for -- and the script stopped with:
#   static-full.sh: FAIL: the artefact has no 'filter_var':
#   examples/http-direct-worker-mysql cannot open a connection without
#   ext-filter. The extension set in the configure line above changed.
symbols=$(strings -a "$artifact")
assert_symbol() {
  echo "$symbols" | grep -qw -- "$1" ||
    fail "the artefact has no '$1': $2. The extension set in the configure line above changed."
}
# ext-filter: league/uri-interfaces calls filter_var($host, FILTER_VALIDATE_IP)
# under amphp/socket's connect(); amphp/dns requires it outright.
assert_symbol filter_var "examples/http-direct-worker-mysql cannot open a connection without ext-filter"
# ext-ctype: daverandom/libdns parses the name amphp/dns resolves.
assert_symbol ctype_digit "amphp/dns cannot resolve a hostname without ext-ctype"
# ext-json: a hard requirement of amphp/dns, and how every worker example
# answers a request.
assert_symbol json_encode "amphp/dns requires ext-json, and the worker examples encode their replies with it"
# ext-openssl: amphp/socket's TLS, and this SAPI's own https listener.
assert_symbol openssl_encrypt "amphp/socket TLS and the pool's http.tls_* listener need ext-openssl"
# The SAPI's own builtins: --enable-fpmng is what this binary exists for, and
# a worker handler cannot answer a request without this one.
assert_symbol fpmng_worker_respond "pool.executor = worker cannot answer a request without the fpmng_worker_* builtins"
echo "static-full.sh: extension-set assertions ok"

# Issue #171: the ACME client goes in after the link and before the artefact is
# copied anywhere. Nothing here strips, which is what makes this safe -- strip(1)
# drops appended data.
/repo/build/embed-payload.sh "$artifact" /build/sapi/cli/php ||
  fail "the distribution payload could not be embedded"

cp "$artifact" /out/php-fpm-ng-full
echo "static-full.sh: PASS"
