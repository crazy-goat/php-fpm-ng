#!/bin/sh
# Assemble the SAPI's C sources from this repository alone (issue #421).
#
#   build/standalone-tree.sh <outdir>
#
# Writes:
#   <outdir>/sapi/fpmng/fpm/   third_party/php-src/sapi/fpm/fpm/ with
#                              sapi/fpmng/fpm/ on top
#   <outdir>/main/             fastcgi.c and fastcgi.h, pristine
#
# WHY A MERGED DIRECTORY rather than two -I paths: upstream's events/*.c
# include "../zlog.h", "../fpm.h" and friends. A quoted include with ".." is
# resolved against the including file's directory, so the event backends only
# find this repo's zlog.h (which wraps upstream's, kept as zlog_upstream.h) if
# both live in one directory. That is the same overlay build/prepare.sh makes
# inside a php-src tree; here it is made from the vendored copy instead, and
# nothing else from php-src is involved. vendor-php-src.sh check guarantees no
# file exists on both sides, so the copy order decides nothing; ours is copied
# last anyway so a future collision cannot silently pick upstream's.
#
# main/ holds ONLY the two FastCGI files on purpose. A build that put a php-src
# tree's main/ on the include path would compile against that tree's php.h,
# php_network.h and streams headers instead of the SDK's -- the header
# shadowing the libphp build had before this script (the fastcgi.c object was
# the one translation unit that read the prepared tree's main/*.h). The SDK's
# own main/fastcgi.h must not be found either: it is not the copy this build pins.
# Callers put <outdir>/main first on the include path, ahead of the SDK.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:?usage: build/standalone-tree.sh <outdir>}
TP=$REPO/third_party/php-src

[ -f "$TP/MANIFEST" ] || { echo "standalone-tree.sh: no third_party/php-src/MANIFEST" >&2; exit 1; }

rm -rf "$OUT/sapi/fpmng/fpm" "$OUT/main"
mkdir -p "$OUT/sapi/fpmng" "$OUT/main"
cp -R "$TP/sapi/fpm/fpm" "$OUT/sapi/fpmng/fpm"
# Only the C and header files: the overlay directory has nothing else today,
# and nothing else belongs in a compile tree.
(cd "$REPO/sapi/fpmng/fpm" && find . -type f \( -name '*.c' -o -name '*.h' \)) |
  while read -r f; do
    mkdir -p "$OUT/sapi/fpmng/fpm/$(dirname "$f")"
    cp "$REPO/sapi/fpmng/fpm/$f" "$OUT/sapi/fpmng/fpm/$f"
  done
cp "$TP/main/fastcgi.c" "$TP/main/fastcgi.h" "$OUT/main/"
