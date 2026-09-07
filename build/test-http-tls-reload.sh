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

# All N samples must equal $2 (the expected serial) -- with http.gateways=3
# and SO_REUSEPORT, this is the only way to be sure every gateway process
# adopted the reload, not just whichever one handled the first connection
# (acceptance criterion 1).
assert_all_serve() {
    port=$1 want=$2 samples=$3 label=$4
    i=0
    while [ "$i" -lt "$samples" ]; do
        got=$(served_serial "$port")
        [ "$got" = "$want" ] || fail "$label: connection $i served serial $got, want $want"
        i=$((i + 1))
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
cp "$DIR/certs/fullchain-leaf2.pem" "$DIR/certs/live-cert.pem"
cp "$DIR/certs/leaf2.key" "$DIR/certs/live-key.pem"

wait "$LOAD_PID" || true
[ -s "$DIR/load.errors" ] && { cat "$DIR/load.errors" >&2; fail "acceptance criterion 2: $(wc -l < "$DIR/load.errors") request(s) failed across the reload"; }
info "acceptance criterion 2: 300 requests across the swap, zero failures"

# http.tls_reload_check=1 (master) + each child's own 1s timer: 4s is a
# generous margin for every one of the 3 gateway processes to have ticked
# at least once since the swap above.
sleep 4

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
cp "$DIR/certs/fullchain-leaf2.pem" "$DIR/certs/live-cert.pem"
cp "$DIR/certs/leaf1.key" "$DIR/certs/live-key.pem"		# leaf2 cert, leaf1 key: mismatch
sleep 4
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
