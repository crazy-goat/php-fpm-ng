#!/bin/sh
# The single-file workflow of docs/guides/single-file-app.md, end to end (issue #431).
#
#   build/test-pack-smoke.sh /path/to/php-fpm-ng [/path/to/php8.5]
#
# Needs a built php-fpm-ng, a PHP 8.5 CLI (only to write the fixture PHAR, never
# to pack or run), curl and an unprivileged user. No compiler, no php-src, no
# Composer, no network. The PHAR is the small hand-written one of
# sapi/fpmng/tests/fpmng-pack-app.inc: this is a test fixture, not a PHAR builder.
#
# Steps: pack the three inputs, delete them, start the output, read code, an
# included file and a resource through HTTP, prove the embedded ini wins over a
# host php.ini, refuse a missing input and a damaged payload, repack over the
# running executable and reload with SIGUSR2, stop with SIGTERM.
# The full lifecycle and negative matrix is sapi/fpmng/tests/fpmng-pack.phpt and
# fpmng-pack-run.phpt; this script is the reproducible smoke test of the chapter.
set -eu

fail() {
    printf 'test-pack-smoke.sh: FAIL: %s\n' "$*" >&2
    exit 1
}

[ "$#" -ge 1 ] || fail "usage: $0 /path/to/php-fpm-ng [php]"
BIN=$1
PHP=${2:-php}
[ -x "$BIN" ] || fail "not executable: $BIN"
command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v "$PHP" >/dev/null 2>&1 || fail "no PHP CLI: $PHP"
[ "$(id -u)" -ne 0 ] || fail "run as an unprivileged user (php-fpm refuses root)"
"$BIN" -v
printf 'binary under test: '
sha256sum "$BIN"
REPO=$(cd "$(dirname "$0")/.." && pwd)
PORT=${FPMNG_SMOKE_PORT:-18090}
if curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$PORT/"; then
    fail "port $PORT is in use; set FPMNG_SMOKE_PORT"
fi

DIR=$(mktemp -d)
PID=
cleanup() {
    set +e
    [ -n "$PID" ] && kill -TERM "$PID" 2>/dev/null && wait "$PID" 2>/dev/null
    rm -rf "$DIR"
}
trap cleanup EXIT INT TERM
mkdir "$DIR/in" "$DIR/state" "$DIR/host"

# The fixture PHAR, version $1 (a letter), written to $DIR/in/app.phar.
write_phar() {
    # shellcheck disable=SC2016 # the PHP program is meant to stay unexpanded
    FIX_V=$1 FIX_INC="$REPO/sapi/fpmng/tests/fpmng-pack-app.inc" FIX_OUT="$DIR/in/app.phar" "$PHP" -n -r '
        require getenv("FIX_INC");
        $v = getenv("FIX_V");
        file_put_contents(getenv("FIX_OUT"), fpmng_mini_phar([
            "public/index.php" => "<?php\nrequire_once __DIR__ . \"/../lib/lib.php\";\n" .
                "spl_autoload_register(function (\$c) { require __DIR__ . \"/../lib/\" . \$c . \".php\"; });\n" .
                "echo \"app $v \", lib_version(), \" \", AutoGreeter::tag(), \" \", trim(file_get_contents(__DIR__ . \"/../res/data.txt\")), \" mem=\", ini_get(\"memory_limit\");\n",
            "lib/lib.php" => "<?php\nfunction lib_version() { return \"lib-$v\"; }\n",
            "lib/AutoGreeter.php" => "<?php\nclass AutoGreeter { public static function tag() { return \"auto-$v\"; } }\n",
            "res/data.txt" => "resource-$v\n",
        ]));'
}

write_inputs() {
    write_phar "$1"
    printf 'extension=phar\nmemory_limit=77M\n' >"$DIR/in/php.ini"
    cat >"$DIR/in/fpm.conf" <<CFG
[global]
daemonize = no
error_log = $DIR/err.log

[web]
listen = 127.0.0.1:$PORT
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = /
http.front_controller = fpmng-app://public/index.php
CFG
}

# The host php.ini would change the answer if the packed executable read it.
printf 'memory_limit=11M\n' >"$DIR/host/php.ini"
export PHPRC="$DIR/host" FPMNG_APP_DIR="$DIR/state"

write_inputs A
"$BIN" pack "$DIR/in/app.phar" "$DIR/in/php.ini" "$DIR/in/fpm.conf" -o "$DIR/app" >"$DIR/pack.out" 2>&1 \
    || { cat "$DIR/pack.out" >&2; fail "pack"; }
printf 'packed output: '
sha256sum "$DIR/app"
echo "ok: packed"

# Mandatory input refusal: nothing is written.
if "$BIN" pack "$DIR/in/app.phar" "$DIR/in/php.ini" "$DIR/in/missing.conf" -o "$DIR/never" >"$DIR/refuse.out" 2>&1; then
    fail "pack accepted a missing fpm.conf"
fi
grep -q 'missing.conf' "$DIR/refuse.out" || { cat "$DIR/refuse.out" >&2; fail "refusal does not name the file"; }
[ ! -e "$DIR/never" ] || fail "a refused pack left an output"
echo "ok: a missing input is refused by name"

# A damaged payload is refused and never falls back to a host configuration.
size=$(wc -c <"$DIR/app")
head -c $((size - 1)) "$DIR/app" >"$DIR/cut"
chmod +x "$DIR/cut"
if "$DIR/cut" >"$DIR/cut.out" 2>&1; then
    fail "a truncated executable started"
fi
grep -qi 'payload' "$DIR/cut.out" || { cat "$DIR/cut.out" >&2; fail "no payload message for a truncated executable"; }
echo "ok: a truncated executable is refused"

# The sources are gone before the first start.
rm -f "$DIR/in/app.phar" "$DIR/in/php.ini" "$DIR/in/fpm.conf"

"$DIR/app" -t >"$DIR/t.out" 2>&1 || { cat "$DIR/t.out" >&2; fail "-t"; }
"$DIR/app" >"$DIR/master.out" 2>&1 &
PID=$!

want() { # want <path> <body>
    body=
    for _ in $(seq 1 100); do
        body=$(curl -fsS --max-time 2 "http://127.0.0.1:$PORT$1" 2>/dev/null) && [ "$body" = "$2" ] && return 0
        kill -0 "$PID" 2>/dev/null || break
        sleep 0.2
    done
    cat "$DIR/master.out" "$DIR/err.log" >&2 2>/dev/null || true
    fail "GET $1: wanted '$2', got '$body'"
}

want / 'app A lib-A auto-A resource-A mem=77M'
echo "ok: code, include, autoload, resource and the embedded php.ini (not the host one)"

# Upgrade: repack, rename over the running executable, SIGUSR2.
write_inputs B
"$BIN" pack "$DIR/in/app.phar" "$DIR/in/php.ini" "$DIR/in/fpm.conf" -o "$DIR/app.new" >"$DIR/pack2.out" 2>&1 \
    || { cat "$DIR/pack2.out" >&2; fail "repack"; }
mv "$DIR/app.new" "$DIR/app"
kill -USR2 "$PID"
want / 'app B lib-B auto-B resource-B mem=77M'
echo "ok: a repack renamed over the executable is served after SIGUSR2"

kill -TERM "$PID"
wait "$PID" 2>/dev/null || true
PID=
echo "ok: stopped"
