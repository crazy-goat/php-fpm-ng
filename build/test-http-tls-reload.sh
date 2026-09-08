#!/bin/sh
# Verifies task 040: the HTTP gateway's TLS certificate/key can be replaced
# on disk and picked up by every gateway process without a restart, without
# dropping connections, and without ever installing a broken candidate (see
# tasks/040-tls-certificate-reload-without-restart.md).
#
# No root needed (unlike test-http-gateway-privileges.sh): this only reads
# TCP ports and PEM files this script itself creates.
#
# Usage:
#   build/test-http-tls-reload.sh /path/to/php-fpm-ng
set -eu

usage() {
    cat >&2 <<'EOF'
Usage: build/test-http-tls-reload.sh /path/to/php-fpm-ng
EOF
    exit 2
}

fail() {
    printf 'test-http-tls-reload.sh: FAIL: %s\n' "$*" >&2
    exit 1
}

info() {
    printf 'test-http-tls-reload.sh: %s\n' "$*"
}

[ "$#" -eq 1 ] || usage
FPMNG_BIN=$1
[ -x "$FPMNG_BIN" ] || fail "not an executable file: $FPMNG_BIN"
case $FPMNG_BIN in
    /*) ;;
    *) FPMNG_BIN=$(pwd)/$FPMNG_BIN ;;
esac

command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v openssl >/dev/null 2>&1 || fail "the openssl CLI is required"

DIR=$(mktemp -d)
trap 'cleanup_all' EXIT INT TERM
MASTER_PID=""

cleanup_all() {
    set +e
    [ -n "$MASTER_PID" ] && kill -TERM "$MASTER_PID" >/dev/null 2>&1
    sleep 0.3
    [ -n "$MASTER_PID" ] && kill -KILL "$MASTER_PID" >/dev/null 2>&1
    rm -rf "$DIR"
}

wait_for_port() {
    port=$1
    i=0
    while [ "$i" -lt 100 ]; do
        if curl --silent --output /dev/null --insecure --connect-timeout 1 "https://127.0.0.1:$port/" >/dev/null 2>&1; then
            return 0
        fi
        kill -0 "$MASTER_PID" 2>/dev/null || return 1
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

served_serial() {
    port=$1
    echo | openssl s_client -connect "127.0.0.1:$port" -CAfile "$DIR/certs/root.crt" 2>/dev/null \
        | openssl x509 -noout -serial 2>/dev/null | sed 's/^serial=//'
}

# GNU coreutils and BSD/macOS disagree on the flag; the script is otherwise
# portable sh and is also run by hand on a dev machine, not only in CI.
mtime_of() {
    stat -c %Y "$1" 2>/dev/null || stat -f %m "$1"
}

# Replaces live-cert.pem/live-key.pem, and does not return until the new
# live-cert.pem has an mtime the master can tell apart from the previous one.
#
# The master detects a renewal by comparing st_mtime in WHOLE SECONDS
# (fpm_http_tls_reload.c:140-142). live-cert.pem was last written by another
# `cp` in this same script, so a swap landing inside that same second changes
# no mtime the master looks at, no tick ever validates the new pair, and the
# reload simply never happens.
#
# This is not a hypothetical. Measured on the poligon 2026-09-08: this script
# without swap_in() failed 2 of 5 runs, both with "post-reload: connection 0
# served the OLD serial" and **zero** gateway adoption notices in the error
# log. That is the same symptom as the CI failures in build-matrix.yml runs
# 34139815139, 34140641442, 34121726613, 34120910345 and 34120774897, which had
# been attributed to a gateway missing its 1s tick. An instrumented copy of the
# version below showed 3 of 8 runs retrying here (the two copies are ~0.6s
# apart, so a collision is close to a coin toss); the fixed script passed 16 of
# 16. Whether a gateway can *also* be late is a separate, unmeasured question.
#
# Whole-second mtime comparison in the product is a real sharp edge (a
# certificate renewed twice inside one second is not picked up), recorded as a
# finding for its own task; changing the reload mechanism is out of scope here.
swap_in() {
    cert=$1 key=$2
    was=$(mtime_of "$DIR/certs/live-cert.pem") ||
        fail "swap: cannot stat $DIR/certs/live-cert.pem"
    i=0
    while [ "$i" -lt 50 ]; do
        cp "$DIR/certs/$cert" "$DIR/certs/live-cert.pem"
        cp "$DIR/certs/$key" "$DIR/certs/live-key.pem"
        [ "$(mtime_of "$DIR/certs/live-cert.pem")" != "$was" ] && return 0
        sleep 0.2
        i=$((i + 1))
    done
    fail "swap: live-cert.pem kept mtime $was across $i copies, the master cannot see this swap"
}

# All N samples must equal $2 (the expected serial) -- with http.gateways=3
# and SO_REUSEPORT, this is the only way to be sure every gateway process
# adopted the reload, not just whichever one handled the first connection
# (acceptance criterion 1).
#
assert_all_serve() {
    port=$1 want=$2 samples=$3 label=$4
    i=0
    while [ "$i" -lt "$samples" ]; do
        got=$(served_serial "$port")
        [ "$got" = "$want" ] || fail "$label: connection $i served serial $got, want $want"
        i=$((i + 1))
    done
}

# Deadline for a reload to have reached every gateway process. 30x
# http.tls_reload_check=1 below, deliberately an order of magnitude above the
# interval rather than a constant tuned to one machine: what the script can
# know is that a healthy reload lands *eventually*, never that it lands within
# any particular number of seconds.
RELOAD_DEADLINE=${FPMNG_TLS_RELOAD_DEADLINE:-30}

# Waits, bounded by RELOAD_DEADLINE, until the error log holds at least $2
# lines matching $1. This is how the fixed `sleep 4`s were replaced, and the
# readiness signal is deliberately the log rather than the wire.
#
# Retrying the wire assertion instead was tried first and is *wrong*: the
# 12-sample burst above is a statistical assertion (3 gateways behind
# SO_REUSEPORT, and the script cannot see which one served a connection), so
# retrying it until it comes up clean inverts it. With one gateway permanently
# stuck a single burst is red with probability 1-(2/3)^12 = 99.2%, but ~75
# retries inside a 30s deadline find a clean burst about 44% of the time --
# i.e. retrying would have quietly turned the exact regression this job exists
# to catch into a coin toss.
#
# The log has no such problem: both notices below are emitted once per process
# per event, so their COUNT is exact. A gateway that never adopts leaves the
# count at 2 of 3 and fails here deterministically, with a message that names
# the number seen -- and the wire assertion that follows is left as strict as
# it was before this task.
wait_for_log_count() {
    pattern=$1 want=$2 label=$3
    deadline=$(($(date +%s) + RELOAD_DEADLINE))
    while :; do
        now=$(grep -c "$pattern" "$DIR/error.log" 2>/dev/null || true)
        if [ "${now:-0}" -ge "$want" ]; then
            return 0
        fi
        if [ "$(date +%s)" -ge "$deadline" ]; then
            fail "$label: waited ${RELOAD_DEADLINE}s for $want log line(s) matching '$pattern', saw ${now:-0}"
        fi
        kill -0 "$MASTER_PID" 2>/dev/null || fail "$label: the master exited while waiting"
        sleep 0.2
    done
}

info "generating a 2-level test CA (root -> intermediate) and two leaf certs"
mkdir -p "$DIR/certs" "$DIR/docroot"
cd "$DIR/certs"

openssl genrsa -out root.key 2048 >/dev/null 2>&1
openssl req -x509 -new -key root.key -sha256 -days 2 -subj /CN=root.test -out root.crt >/dev/null 2>&1

openssl genrsa -out inter.key 2048 >/dev/null 2>&1
openssl req -new -key inter.key -subj /CN=intermediate.test -out inter.csr >/dev/null 2>&1
printf 'basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n' > inter.ext
openssl x509 -req -in inter.csr -CA root.crt -CAkey root.key -CAcreateserial -days 2 -sha256 -extfile inter.ext -out inter.crt >/dev/null 2>&1

printf 'basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\n' > leaf.ext
for leaf in leaf1 leaf2; do
    openssl genrsa -out "$leaf.key" 2048 >/dev/null 2>&1
    openssl req -new -key "$leaf.key" -subj "/CN=$leaf.test" -out "$leaf.csr" >/dev/null 2>&1
    openssl x509 -req -in "$leaf.csr" -CA inter.crt -CAkey inter.key -CAcreateserial -days 2 -sha256 -extfile leaf.ext -out "$leaf.crt" >/dev/null 2>&1
    cat "$leaf.crt" inter.crt > "fullchain-$leaf.pem"
done

SERIAL1=$(openssl x509 -in leaf1.crt -noout -serial | sed 's/^serial=//')
SERIAL2=$(openssl x509 -in leaf2.crt -noout -serial | sed 's/^serial=//')
[ "$SERIAL1" != "$SERIAL2" ] || fail "test setup: leaf1 and leaf2 got the same serial"

cp fullchain-leaf1.pem live-cert.pem
cp leaf1.key live-key.pem

cd "$DIR"
cat > docroot/index.php <<'EOF'
<?php echo "ok"; ?>
EOF

HTTP_PORT=19340
FCGI_PORT=19240
cat > fpm.conf <<EOF
[global]
daemonize = no
error_log = $DIR/error.log
pid = $DIR/fpm.pid
log_level = notice
[www]
chdir = $DIR/docroot
listen = 127.0.0.1:$FCGI_PORT
pm = static
pm.max_children = 4
pool.type = http
http.gateways = 3
http.reuseport = yes
http.listen = 127.0.0.1:$HTTP_PORT
http.front_controller = /index.php
http.tls_cert = $DIR/certs/live-cert.pem
http.tls_key = $DIR/certs/live-key.pem
http.tls_reload_check = 1
EOF

info "starting php-fpm-ng (3 gateways, SO_REUSEPORT, http.tls_reload_check=1)"
"$FPMNG_BIN" -n -R -F -y "$DIR/fpm.conf" > "$DIR/stdout.log" 2>&1 &
MASTER_PID=$!

wait_for_port "$HTTP_PORT" || {
    cat "$DIR/error.log" "$DIR/stdout.log" 2>/dev/null >&2
    fail "gateway never accepted a connection on 127.0.0.1:$HTTP_PORT"
}

info "baseline: every gateway process serves leaf1 ($SERIAL1)"
assert_all_serve "$HTTP_PORT" "$SERIAL1" 8 "baseline"

body=$(curl --silent --show-error --insecure --connect-timeout 2 --max-time 5 "https://127.0.0.1:$HTTP_PORT/")
[ "$body" = "ok" ] || fail "expected body 'ok' before reload, got: $body"

# Acceptance criterion 2: connections in flight across the swap complete
# normally. A background loop of real HTTPS requests, started before the
# file swap and still running well after it.
: > "$DIR/load.errors"
(
    i=0
    while [ "$i" -lt 300 ]; do
        out=$(curl --silent --show-error --insecure --connect-timeout 2 --max-time 5 "https://127.0.0.1:$HTTP_PORT/" 2>&1) || true
        [ "$out" = "ok" ] || echo "request $i: $out" >> "$DIR/load.errors"
        i=$((i + 1))
    done
) &
LOAD_PID=$!

sleep 0.3
info "swapping in leaf2 ($SERIAL2) while the load loop is running"
swap_in fullchain-leaf2.pem leaf2.key

wait "$LOAD_PID" || true
[ -s "$DIR/load.errors" ] && { cat "$DIR/load.errors" >&2; fail "acceptance criterion 2: $(wc -l < "$DIR/load.errors") request(s) failed across the reload"; }
info "acceptance criterion 2: 300 requests across the swap, zero failures"

# One notice per gateway process (fpm_http_tls_reload.c:327), so 3 of them is
# every gateway of this pool having adopted the new generation.
wait_for_log_count "adopted reloaded TLS certificate" 3 "post-reload"

info "acceptance criterion 1: every one of the 3 gateway processes now serves leaf2 ($SERIAL2)"
assert_all_serve "$HTTP_PORT" "$SERIAL2" 12 "post-reload"

info "acceptance criterion 5: session resumption still works (shared ticket_key survives the reload)"
openssl s_client -connect "127.0.0.1:$HTTP_PORT" -CAfile "$DIR/certs/root.crt" -tls1_2 \
    -sess_out "$DIR/sess.pem" </dev/null >/dev/null 2>&1
[ -s "$DIR/sess.pem" ] || fail "could not save a TLS session to test resumption with"
i=0
while [ "$i" -lt 5 ]; do
    reused=$(openssl s_client -connect "127.0.0.1:$HTTP_PORT" -CAfile "$DIR/certs/root.crt" -tls1_2 \
        -sess_in "$DIR/sess.pem" </dev/null 2>&1 | grep -c '^Reused,') || true
    [ "$reused" = "1" ] || fail "session resumption attempt $i was not reused (got a full handshake instead)"
    i=$((i + 1))
done

info "acceptance criterion 3: a certificate/key that do not match each other is rejected, old certificate keeps serving"
REJECTIONS_BEFORE=$(grep -c "http.tls_cert/http.tls_key:" "$DIR/error.log" 2>/dev/null || true)
swap_in fullchain-leaf2.pem leaf1.key		# leaf2 cert, leaf1 key: mismatch
# Waiting for the rejection notice instead of sleeping is also strictly
# stronger than the `sleep 4` it replaces: the master rejects the pair without
# bumping the shared generation (fpm_http_tls_reload.c:155-161), so once this
# line exists there is nothing left for a gateway to adopt and the serials
# below are settled -- whereas the fixed sleep asserted at an arbitrary point
# that might have been before the tick ran at all.
wait_for_log_count "http.tls_cert/http.tls_key:" "$((${REJECTIONS_BEFORE:-0} + 1))" "broken candidate"
assert_all_serve "$HTTP_PORT" "$SERIAL2" 6 "after a broken candidate"
grep -q "http.tls_cert/http.tls_key:" "$DIR/error.log" || fail "no error naming the broken candidate was logged"
body=$(curl --silent --show-error --insecure --connect-timeout 2 --max-time 5 "https://127.0.0.1:$HTTP_PORT/")
[ "$body" = "ok" ] || fail "gateway stopped accepting after a broken candidate, got: $body"

info "acceptance criterion 4: no key material anywhere in the logs"
if grep -l "BEGIN.*PRIVATE KEY" "$DIR/error.log" "$DIR/stdout.log" >/dev/null 2>&1; then
    fail "found what looks like key material in the logs"
fi

kill -TERM "$MASTER_PID"
wait "$MASTER_PID" 2>/dev/null || true
MASTER_PID=""

info "all scenarios PASS"
