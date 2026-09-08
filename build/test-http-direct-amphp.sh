#!/bin/sh
# Task 073: proves that UNMODIFIED revolt/event-loop + amphp/amp from Composer
# run on pool.type = http-direct + pool.executor = worker, and that N
# concurrent requests to a handler doing Amp\delay(1) finish in roughly one
# second on a single worker instead of N seconds.
#
# The only project code involved is examples/http-direct-worker/: a userland
# Revolt driver over the fpmng_worker_* primitives plus a hello-world app. If
# this passes, the SAPI event surface is complete enough for the whole amphp
# ecosystem, which is the acceptance proof task 072 asked for.
#
# Not wired into CI: it needs Composer and network. It SKIPS (exit 0) rather
# than failing when either is missing, so it is safe to call unconditionally.
# The dependency-free half of the same claim is
# sapi/fpmng/tests/fpmng-http-direct-worker.phpt, which CI does run.
#
# Usage:
#   ./build/test-http-direct-amphp.sh /path/to/php-fpm-ng [/path/to/php-cli]
#
# Environment:
#   FPMNG_AMPHP_VENDOR  Pre-built vendor/ directory to use instead of running
#                       Composer. For a box with no network: run Composer
#                       elsewhere and copy the tree over.
#   FPMNG_COMPOSER      Path to composer.phar, for a box where Composer is not
#                       installed system-wide (the test box is one).
#   FPMNG_AMPHP_PORT    Base port (default 28074). The box is shared; pick your
#                       own range.
#   FPMNG_AMPHP_N       Concurrent /sleep requests (default 8).
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
EXAMPLE="$REPO/examples/http-direct-worker"
PORT=${FPMNG_AMPHP_PORT:-28074}
N=${FPMNG_AMPHP_N:-8}

fail() {
    printf 'test-http-direct-amphp.sh: FAIL: %s\n' "$*" >&2
    exit 1
}
skip() {
    printf 'test-http-direct-amphp.sh: SKIP: %s\n' "$*"
    exit 0
}

[ "$#" -ge 1 ] || fail "usage: $0 /path/to/php-fpm-ng [/path/to/php-cli]"
FPMNG_BIN=$1
[ -x "$FPMNG_BIN" ] || fail "not executable: $FPMNG_BIN"

# workflow.md: confirm the binary under test before measuring anything. This
# literal is the worker transport's SERVER_SOFTWARE; a build without
# fpm_http_direct_worker.c linked in does not contain it, and the pool would
# fail with "unknown pool.executor" much later and less clearly.
strings "$FPMNG_BIN" 2>/dev/null | grep -q 'php-fpm-ng/http-direct-worker' ||
    fail "$FPMNG_BIN does not contain the worker transport; rebuild after buildconf --force (build/prepare.sh warning)"

command -v curl >/dev/null 2>&1 || fail "curl is required"

DIR=$(mktemp -d)
MASTER_PID=
trap 'set +e; [ -n "$MASTER_PID" ] && kill -TERM "$MASTER_PID" 2>/dev/null; rm -rf "$DIR"' EXIT INT TERM

mkdir "$DIR/app"
cp "$EXAMPLE/app.php" "$EXAMPLE/FpmngDriver.php" "$EXAMPLE/FpmngServer.php" \
   "$EXAMPLE/composer.json" "$DIR/app/"

if [ -n "${FPMNG_AMPHP_VENDOR:-}" ]; then
    [ -f "$FPMNG_AMPHP_VENDOR/autoload.php" ] ||
        fail "FPMNG_AMPHP_VENDOR=$FPMNG_AMPHP_VENDOR has no autoload.php"
    cp -R "$FPMNG_AMPHP_VENDOR" "$DIR/app/vendor"
else
    PHP_CLI=${2:-}
    if [ -z "$PHP_CLI" ]; then
        PHP_CLI=$(command -v php 2>/dev/null || true)
    fi
    [ -n "$PHP_CLI" ] && [ -x "$PHP_CLI" ] ||
        skip "no PHP CLI to run Composer with; pass one as \$2 or set FPMNG_AMPHP_VENDOR"
    COMPOSER=${FPMNG_COMPOSER:-$(command -v composer 2>/dev/null || true)}
    [ -n "$COMPOSER" ] && [ -f "$COMPOSER" ] ||
        skip "composer not found; set FPMNG_COMPOSER or FPMNG_AMPHP_VENDOR"
    # --no-interaction so a missing network fails fast instead of prompting.
    if ! (cd "$DIR/app" && "$PHP_CLI" "$COMPOSER" install \
            --no-interaction --no-progress --quiet >"$DIR/composer.log" 2>&1); then
        cat "$DIR/composer.log" >&2
        skip "composer install failed (no network?), see the log above"
    fi
fi

# Report which versions actually ran: "unmodified amphp" is only a meaningful
# claim with the versions named.
if [ -f "$DIR/app/vendor/composer/installed.json" ]; then
    grep -o '"name": *"\(amphp\|revolt\)/[^"]*"\|"version": *"[^"]*"' \
        "$DIR/app/vendor/composer/installed.json" | paste - - 2>/dev/null | head -20 || true
fi

cat > "$DIR/fpm.conf" <<EOF
[global]
daemonize = no
error_log = $DIR/error.log
[amphp]
listen = 127.0.0.1:$PORT
pool.type = http-direct
pool.executor = worker
pm = static
; One worker on purpose: any overlap observed below is concurrency INSIDE a
; single process, not FPM spreading the load over children.
pm.max_children = 1
chdir = $DIR/app
http.front_controller = /app.php
http.read_timeout = 30000
http.max_body = 1M
catch_workers_output = yes
php_admin_value[max_execution_time] = 0
EOF

"$FPMNG_BIN" -n -R -F -y "$DIR/fpm.conf" >"$DIR/stdout.log" 2>&1 &
MASTER_PID=$!
i=0
while ! curl -s --connect-timeout 1 "http://127.0.0.1:$PORT/" >/dev/null 2>&1; do
    kill -0 "$MASTER_PID" 2>/dev/null || {
        cat "$DIR/stdout.log" "$DIR/error.log" >&2 2>/dev/null
        fail "master exited during startup"
    }
    [ "$i" -lt 100 ] || fail "worker did not start serving; see $DIR/stdout.log"
    i=$((i + 1))
    sleep 0.1
done

hello=$(curl -s "http://127.0.0.1:$PORT/")
case $hello in
    'hello world from pid '*) ;;
    *) fail "unexpected hello world body: $hello" ;;
esac
printf 'hello-world: ok (%s)\n' "$hello"

# N concurrent /sleep, each Amp\delay(1.0) inside the one worker.
start=$(date +%s)
j=1
clients=
while [ "$j" -le "$N" ]; do
    curl -s "http://127.0.0.1:$PORT/sleep?id=$j" > "$DIR/body.$j" &
    clients="$clients $!"
    j=$((j + 1))
done
# Only the clients. A bare `wait` also waits for the php-fpm master started
# with & above, which never exits on its own — that hung the whole harness
# after a passing measurement.
for pid in $clients; do
    wait "$pid" || fail "client $pid failed"
done
end=$(date +%s)
elapsed=$((end - start))

j=1
while [ "$j" -le "$N" ]; do
    grep -q '"id"' "$DIR/body.$j" ||
        fail "request $j did not return JSON: $(cat "$DIR/body.$j")"
    j=$((j + 1))
done

# Serialized would be N seconds. The bound is deliberately loose (curl startup
# plus scheduling on a shared box), but it still separates the two cases for
# any N >= 4: 3 s cannot be reached by N serialized one-second sleeps.
[ "$elapsed" -le 3 ] ||
    fail "$N concurrent Amp\\delay(1) took ${elapsed}s; serialized, not concurrent"
printf 'concurrent-delay: ok (%s requests in %ss on one worker)\n' "$N" "$elapsed"

# Same pid throughout: the booted script was never restarted.
pids=$(cat "$DIR"/body.* | grep -o '"pid":[0-9]*' | sort -u | wc -l | tr -d ' ')
[ "$pids" = 1 ] || fail "batch was served by $pids distinct workers"
printf 'single-worker: ok\n'

kill -TERM "$MASTER_PID" 2>/dev/null || true
wait "$MASTER_PID" 2>/dev/null || true
MASTER_PID=
# `cmd && fail` would trip `set -e` on the no-match case, which is the PASSING
# one here, so this is spelled as an if.
if diagnostics=$(grep -Ei 'PHP (Fatal|Warning|Notice)|Uncaught' \
        "$DIR/stdout.log" "$DIR/error.log" 2>/dev/null); then
    fail "PHP diagnostics in the logs: $diagnostics"
fi

printf 'test-http-direct-amphp.sh: PASS\n'
