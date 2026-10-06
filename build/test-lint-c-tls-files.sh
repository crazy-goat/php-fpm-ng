#!/usr/bin/env bash
# build/lint-c.sh must skip the TLS-only translation unit when the build that
# supplied the flags was built without TLS (issue #683). Hermetic: fake
# commands.log files in the shape build/libphp-build.sh writes, plus a
# clang-tidy stand-in that prints the .c files it was given. The file list
# itself comes from the real tree, so a renamed TLS file fails this; no
# compiler, no SDK, no network.
#
# The invariant, not the literal: a non-TLS commands.log (no
# -DHAVE_FPM_HTTP_TLS on its compile lines) lints every translation unit
# except sapi/fpmng/fpm/fpm_tls_http_direct.c and says so, while a TLS one
# lints that file too. The skipped file must stay visible: a lint pass that
# silently stops looking at files is how the #683 finding survived.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

TLS_ONLY=sapi/fpmng/fpm/fpm_tls_http_direct.c

fail() { echo "FAIL: $*" >&2; exit 1; }

tmp=$(mktemp -d) || fail "mktemp failed"
# Expand now: the trap runs after this function scope ends, and with `set -u`
# a local would already be unset there.
# shellcheck disable=SC2064
trap "rm -rf '$tmp'" EXIT
mkdir -p "$tmp/fakebin" "$tmp/out-notls" "$tmp/out-tls" || fail "mkdir failed"

# A clang-tidy stand-in: print each .c translation unit it was given.
cat > "$tmp/fakebin/clang-tidy" <<'EOF'
#!/bin/sh
for a in "$@"; do case "$a" in *.c) printf '%s\n' "$a" ;; esac; done
exit 0
EOF
chmod +x "$tmp/fakebin/clang-tidy" || fail "chmod failed"

# Minimal commands.log in the shape build/libphp-build.sh writes: header
# comments, then compile lines carrying the same -D set (lint-c.sh reads the
# first). The TLS flavour carries -DHAVE_FPM_HTTP_TLS=1 because
# libphp-build.sh puts it into DEFS only with FPMNG_TLS=1
# (build/libphp-build.sh:196-199); the link line never matters here.
cat > "$tmp/out-notls/commands.log" <<'EOF'
# Linux x86_64; gcc 13
# php-config8.5 -> PHP 8.5.0, /usr/include/php/20240924
# FPMNG_TLS=0 FPMNG_ACME=0 FPMNG_DEBUG_CLOCK=1 EXTRA_CFLAGS=
gcc -D_GNU_SOURCE -O2 -DHAVE_CONFIG_H -DHAVE_EPOLL=1 -DHAVE_FPM_HTTP=1 -DFPMNG_LIBPHP_BUILD=1 -I/tree/main -c /tree/sapi/fpmng/fpm/fpm.c -o /out/obj/x.o
EOF
cat > "$tmp/out-tls/commands.log" <<'EOF'
# Linux x86_64; gcc 13
# php-config8.5 -> PHP 8.5.0, /usr/include/php/20240924
# FPMNG_TLS=1 FPMNG_ACME=1 FPMNG_DEBUG_CLOCK=1 EXTRA_CFLAGS=
gcc -D_GNU_SOURCE -O2 -DHAVE_CONFIG_H -DHAVE_EPOLL=1 -DHAVE_FPM_HTTP=1 -DHAVE_FPM_HTTP_TLS=1 -DHAVE_FPMNG_ACME=1 -DFPMNG_LIBPHP_BUILD=1 -I/tree/main -c /tree/sapi/fpmng/fpm/fpm.c -o /out/obj/x.o
gcc -o /out/php-fpm-ng /out/obj/x.o -lphp8.5 -levent -levent_openssl -lssl -lcrypto -lm -Wl,-E
EOF

run_lint() {
    PATH="$tmp/fakebin:$PATH" sh build/lint-c.sh "$1"
}

notls=$(run_lint "$tmp/out-notls") || fail "lint-c.sh failed against the non-TLS log"
tls=$(run_lint "$tmp/out-tls") || fail "lint-c.sh failed against the TLS log"

# The TLS-only file is skipped against the non-TLS build, visibly.
printf '%s\n' "$notls" | grep -q "skipping.*$TLS_ONLY" \
    || fail "no visible skip of $TLS_ONLY against the non-TLS log"
printf '%s\n' "$notls" | grep -q "/$TLS_ONLY\$" \
    && fail "$TLS_ONLY was linted against the non-TLS log"
# ... while a TLS build still lints it, silently (nothing skipped).
printf '%s\n' "$tls" | grep -q "/$TLS_ONLY\$" \
    || fail "$TLS_ONLY was not linted against the TLS log"
printf '%s\n' "$tls" | grep -q "skipping" \
    && fail "TLS build skips a file it must lint"
# The guarded TLS siblings lint as empty translation units in both builds,
# and the ACME file (stubbed in its header without the macro) is untouched.
for f in sapi/fpmng/fpm/fpm_tls_http.c sapi/fpmng/fpm/fpm_tls_reload.c \
    sapi/fpmng/fpm/fpm_acme_challenge.c sapi/fpmng/fpm/fpm_http_direct_tls.c; do
    printf '%s\n' "$notls" | grep -q "/$f\$" || fail "$f missing against the non-TLS log"
    printf '%s\n' "$tls" | grep -q "/$f\$" || fail "$f missing against the TLS log"
done

echo "lint-c.sh skips only $TLS_ONLY, and only against a non-TLS build"
