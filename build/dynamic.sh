#!/bin/sh
set -e
apk add --no-cache build-base autoconf bison re2c pkgconf libevent-dev libevent-static linux-headers >/dev/null 2>&1
cd /src
[ -f configure ] || ./buildconf --force
cd /build
[ -f Makefile ] || LDFLAGS="-static" /src/configure \
  --disable-all --enable-fpm --with-fpm-http \
  --without-pear --disable-cgi --disable-phpdbg --disable-shared \
  --prefix=/usr/local > /out/configure.log 2>&1
make -j8 > /out/make.log 2>&1 || { echo "=== BLAD BUDOWANIA ==="; grep -iE "error|undefined ref|cannot find" /out/make.log | head -20; exit 1; }
echo "=== OK ==="
ls -la /build/sapi/fpm/php-fpm
file /build/sapi/fpm/php-fpm
cp /build/sapi/fpm/php-fpm /out/php-fpm-static
