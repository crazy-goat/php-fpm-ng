#!/bin/sh
set -e
apk add --no-cache build-base autoconf bison re2c pkgconf linux-headers \
  libevent-dev libevent-static openssl-dev openssl-libs-static \
  zlib-dev zlib-static curl-dev curl-static libxml2-dev libxml2-static \
  sqlite-dev sqlite-static xz-static brotli-static nghttp2-static nghttp3-static \
  ngtcp2-static zstd-static libidn2-static libpsl-static libunistring-static \
  >/tmp/apk.log 2>&1 || { echo "=== BRAK PAKIETOW ==="; tail -5 /tmp/apk.log; exit 1; }
cd /src
[ -f configure ] || ./buildconf --force
cd /build
export PKG_CONFIG="pkg-config --static"
LDFLAGS="-static-pie" /src/configure \
  --disable-all --enable-fpm --with-fpm-http \
  --enable-opcache --enable-mbstring --disable-mbregex \
  --enable-sockets --enable-pcntl --enable-posix \
  --with-curl --with-openssl --with-zlib --enable-pdo --with-pdo-mysql=mysqlnd \
  --without-pear --disable-cgi --disable-phpdbg --disable-shared \
  --prefix=/usr/local > /out/configure-full.log 2>&1 || { echo "=== BLAD CONFIGURE ==="; tail -12 /out/configure-full.log; exit 1; }
make -j8 > /out/make-full.log 2>&1 || { echo "=== BLAD BUDOWANIA ==="; grep -iE "error|undefined reference|cannot find -l" /out/make-full.log | sort -u | head -25; exit 1; }
echo "=== OK ==="
file /build/sapi/fpm/php-fpm
cp /build/sapi/fpm/php-fpm /out/php-fpm-full
