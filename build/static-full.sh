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
export PKG_CONFIG="pkg-config --static"
LDFLAGS="-static-pie" /src/configure \
  --disable-all --enable-fpmng \
  --enable-opcache --enable-mbstring --disable-mbregex \
  --enable-sockets --enable-pcntl --enable-posix \
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
