#!/bin/sh
# Regression test for cumulative patch application in prepare.sh.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/repo/build" "$TMP/repo/patches" "$TMP/repo/sapi/fpmng/fpm"
mkdir -p "$TMP/php-src/main" "$TMP/php-src/sapi/fpm/fpm"
cp "$REPO/build/prepare.sh" "$TMP/repo/build/"

cat > "$TMP/php-src/main/php_version.h" <<'EOF'
#define PHP_VERSION "8.6.0-dev"
EOF
cat > "$TMP/php-src/main/stack.txt" <<'EOF'
upstream
EOF
cat > "$TMP/php-src/sapi/fpm/config.m4" <<'EOF'
PHP_FPM_FILES="
  fpm/upstream.c \
"
EOF
touch "$TMP/php-src/sapi/fpm/fpm/upstream.c"
# prepare.sh keeps this php-src's zlog.h as sapi/fpmng/fpm/zlog_upstream.h
# for our own zlog.h to include (issue #130), so the fake tree needs one.
touch "$TMP/php-src/sapi/fpm/fpm/zlog.h"
cat > "$TMP/repo/sapi/fpmng/config.m4" <<'EOF'
PHP_FPMNG_FILES="
@FPMNG_SOURCES@
"
PHP_FPMNG_TLS_FILES="
@FPMNG_TLS_SOURCES@
"
PHP_FPMNG_ACME_FILES="
@FPMNG_ACME_SOURCES@
"
EOF
# prepare.sh refuses to run unless it finds at least one fpm_tls_*.c and one
# fpm_acme_*.c file in the source list (build/prepare.sh's own exit-1 guards) --
# these two stand in for them, the same way issue #373 removed the fiber/coop
# files this fake tree used to touch here for the same reason.
touch "$TMP/repo/sapi/fpmng/fpm/fpm_tls_stub.c"
touch "$TMP/repo/sapi/fpmng/fpm/fpm_acme_stub.c"

cat > "$TMP/repo/patches/0001-first.patch" <<'EOF'
--- a/main/stack.txt
+++ b/main/stack.txt
@@ -1 +1,2 @@
 upstream
+first
EOF
cat > "$TMP/repo/patches/0002-dependent.patch" <<'EOF'
--- a/main/stack.txt
+++ b/main/stack.txt
@@ -1,2 +1,3 @@
 upstream
 first
+second
EOF

output=$("$TMP/repo/build/prepare.sh" "$TMP/php-src")
test "$(cat "$TMP/php-src/main/stack.txt")" = "$(printf 'upstream\nfirst\nsecond')"
echo "$output" | grep -q 'patch applied onto upstream: 0002-dependent.patch'

output=$("$TMP/repo/build/prepare.sh" "$TMP/php-src")
echo "$output" | grep -q 'patch was already applied: 0002-dependent.patch'
