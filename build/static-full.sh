#!/bin/sh
set -eu

fail() {
  echo "static-full.sh: FAIL: $*" >&2
  exit 1
}

apk add --no-cache build-base autoconf bison re2c pkgconf linux-headers \
  libevent-dev libevent-static openssl-dev openssl-libs-static \
  zlib-dev zlib-static curl-dev curl-static libxml2-dev libxml2-static \
  sqlite-dev sqlite-static xz-static brotli-static nghttp2-static nghttp3-static \
  ngtcp2-static zstd-static libidn2-static libpsl-static libunistring-static \
  file \
  >/tmp/apk.log 2>&1 || { echo "=== MISSING PACKAGES ==="; tail -5 /tmp/apk.log; exit 1; }
cd /src
[ -f configure ] || ./buildconf --force
cd /build
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
export PKG_CONFIG="pkg-config --static"
LDFLAGS="-static-pie" /src/configure \
  --disable-all --enable-fpmng \
  --enable-opcache --enable-mbstring --disable-mbregex \
  --enable-sockets --enable-pcntl --enable-posix \
  --enable-filter --enable-ctype \
  --with-curl --with-openssl --with-zlib --enable-pdo --with-pdo-mysql=mysqlnd \
  --without-pear --disable-cgi --disable-phpdbg --disable-shared \
  --prefix=/usr/local > /out/configure-full.log 2>&1 || { echo "=== CONFIGURE FAILED ==="; tail -12 /out/configure-full.log; exit 1; }
grep -q 'S\["LIBEVENT_OPENSSL_LIBS"\]="[^"]*event_openssl' /build/config.status ||
  fail "libevent_openssl was not detected; TLS support would be missing"
make -j"${JOBS:-8}" fpmng > /out/make-full.log 2>&1 || {
  echo "=== BUILD FAILED ==="
  grep -iE "error|undefined reference|cannot find -l" /out/make-full.log | sort -u | head -25
  exit 1
}

artifact=/build/sapi/fpmng/php-fpm-ng
[ -x "$artifact" ] || fail "expected artefact is missing: $artifact"
file_output=$(file "$artifact")
echo "$file_output"
echo "$file_output" | grep -q 'static-pie linked' ||
  fail "artefact is not static-pie linked"
cp "$artifact" /out/php-fpm-ng-full
echo "static-full.sh: PASS"
