#!/bin/sh
# Issue #90: a gateway process must survive its own per-connection read
# deadline (task 031, fpm_http_read_deadline_*) expiring on connections that
# were opened and abandoned without a request ever completing.
#
# The crash it reproduces: evhttp frees the bufferevent of an abandoned
# connection and closes its fd; epoll silently drops the deadline's EOF
# watcher along with that fd, so the watcher never fires and the node lives
# on; http.read_timeout later the deadline fires and calls
# bufferevent_set_timeouts() on freed memory. On the poligon that was a
# SIGSEGV inside libevent and a "killed by signal 11, respawning" line from
# the master.
#
# Each round opens a burst of TLS connections that complete the handshake and
# then close without sending a request, waits out http.read_timeout with the
# pool otherwise idle, and then checks that no gateway process died. The
# gateway pids are compared directly rather than only grepping the log: a
# process that dies and is respawned changes them even if the master's line
# ever changes wording.
#
# No root needed: TCP ports on the loopback and files this script creates.
#
# Usage:
#   build/test-http-read-deadline.sh /path/to/php-fpm-ng [rounds]
set -eu

usage() {
    cat >&2 <<'EOF'
Usage: build/test-http-read-deadline.sh /path/to/php-fpm-ng [rounds]
EOF
    exit 2
}

fail() {
    printf 'test-http-read-deadline.sh: FAIL: %s\n' "$*" >&2
    exit 1
}

info() {
    printf 'test-http-read-deadline.sh: %s\n' "$*"
}

[ "$#" -ge 1 ] || usage
[ "$#" -le 2 ] || usage
FPMNG_BIN=$1
ROUNDS=${2:-20}
[ -x "$FPMNG_BIN" ] || fail "not an executable file: $FPMNG_BIN"
case $FPMNG_BIN in
    /*) ;;
    *) FPMNG_BIN=$(pwd)/$FPMNG_BIN ;;
esac

command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v openssl >/dev/null 2>&1 || fail "the openssl CLI is required"

DIR=$(mktemp -d)
MASTER_PID=""
trap 'cleanup_all' EXIT INT TERM

cleanup_all() {
    set +e
    [ -n "$MASTER_PID" ] && kill -TERM "$MASTER_PID" >/dev/null 2>&1
    sleep 0.3
    [ -n "$MASTER_PID" ] && kill -KILL "$MASTER_PID" >/dev/null 2>&1
    rm -rf "$DIR"
}

HTTP_PORT=${FPMNG_HTTP_PORT:-19360}
FCGI_PORT=${FPMNG_FCGI_PORT:-19260}
# Short on purpose: the round below has to wait it out 20 times. The failure
# has no timing component of its own -- the deadline fires late whatever its
# length is -- so a second buys nothing here.
READ_TIMEOUT_MS=1000
# One burst of abandoned connections per round. 40 is what the original
# reproduction on the poligon used.
BURST=${FPMNG_READ_DEADLINE_BURST:-40}

wait_for_port() {
    i=0
    while [ "$i" -lt 100 ]; do
        if curl --silent --output /dev/null --insecure --connect-timeout 1 "https://127.0.0.1:$HTTP_PORT/" >/dev/null 2>&1; then
            return 0
        fi
        kill -0 "$MASTER_PID" 2>/dev/null || return 1
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

gateway_pids() {
    ps -A -o pid=,ppid=,args= 2>/dev/null | awk -v m="$MASTER_PID" '$2 == m && /http gateway/ { print $1 }' | sort
}

info "generating a self-signed certificate for the gateway"
mkdir -p "$DIR/certs" "$DIR/docroot"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=localhost \
    -keyout "$DIR/certs/key.pem" -out "$DIR/certs/cert.pem" >/dev/null 2>&1 ||
    fail "could not generate a test certificate"

cat > "$DIR/docroot/index.php" <<'EOF'
<?php echo "ok"; ?>
EOF

cat > "$DIR/fpm.conf" <<EOF
[global]
daemonize = no
error_log = $DIR/error.log
pid = $DIR/fpm.pid
log_level = notice
[gw]
pool.type = gateway
listen = 127.0.0.1:$HTTP_PORT
chdir = $DIR/docroot
http.gateways = 2
http.reuseport = yes
http.front_controller = /index.php
http.read_timeout = $READ_TIMEOUT_MS
http.tls_cert = $DIR/certs/cert.pem
http.tls_key = $DIR/certs/key.pem
http.route[www] = /

[www]
pool.type = fastcgi
chdir = $DIR/docroot
listen = 127.0.0.1:$FCGI_PORT
pm = static
pm.max_children = 4
EOF

info "starting php-fpm-ng (2 gateways, TLS, http.read_timeout=${READ_TIMEOUT_MS}ms)"
"$FPMNG_BIN" -n -R -F -y "$DIR/fpm.conf" > "$DIR/stdout.log" 2>&1 &
MASTER_PID=$!

wait_for_port || {
    cat "$DIR/error.log" "$DIR/stdout.log" 2>/dev/null >&2
    fail "gateway never accepted a connection on 127.0.0.1:$HTTP_PORT"
}

PIDS_BEFORE=$(gateway_pids)
[ "$(printf '%s\n' "$PIDS_BEFORE" | grep -c '[0-9]')" = "2" ] ||
    fail "expected 2 gateway processes at startup, found: $(printf '%s' "$PIDS_BEFORE" | tr '\n' ' ')"

info "$ROUNDS rounds of $BURST abandoned TLS connections + an idle window past http.read_timeout"
round=0
while [ "$round" -lt "$ROUNDS" ]; do
    # Handshake, then close with no request on the wire: exactly the
    # connections whose read deadline is still armed when evhttp drops them.
    # Concurrently, so the round leaves a burst of deadlines armed at once
    # rather than one at a time.
    rm -rf "$DIR/burst"
    mkdir -p "$DIR/burst"
    i=0
    pids=""
    while [ "$i" -lt "$BURST" ]; do
        openssl s_client -connect "127.0.0.1:$HTTP_PORT" -no_ign_eof \
            </dev/null > "$DIR/burst/$i.out" 2>&1 &
        pids="$pids $!"
        i=$((i + 1))
    done
    # Only these pids: $MASTER_PID is a child of this script too, and a bare
    # `wait` would sit on it until the pool exits.
    for p in $pids; do
        wait "$p" || true
    done

    # Asserted, not assumed. Every failure above is swallowed (an aborted
    # handshake is not interesting in itself), so without this the whole
    # script passes on a binary with the bug the moment openssl rejects one
    # of these flags: 800 no-ops in a millisecond each, two gateway pids that
    # never had a reason to move, PASS.
    handshakes=$(grep -l "Verify return code" "$DIR/burst"/*.out 2>/dev/null | wc -l | tr -d ' ')
    if [ "${handshakes:-0}" != "$BURST" ]; then
        head -20 "$DIR/burst/0.out" >&2 2>/dev/null || true
        fail "round $round: ${handshakes:-0} of $BURST connections completed a TLS handshake (first attempt's output above)"
    fi

    # The deadline fires http.read_timeout after each connection was
    # accepted; the extra second covers the burst still being drained.
    sleep 2

    now=$(gateway_pids)
    if [ "$now" != "$PIDS_BEFORE" ]; then
        grep -E "killed by signal|respawning" "$DIR/error.log" >&2 || true
        fail "round $round: the gateway processes changed ([$(printf '%s' "$PIDS_BEFORE" | tr '\n' ' ')] -> [$(printf '%s' "$now" | tr '\n' ' ')]) -- a gateway died and was respawned"
    fi
    if grep -q "killed by signal" "$DIR/error.log"; then
        grep "killed by signal" "$DIR/error.log" >&2
        fail "round $round: the master logged a gateway killed by a signal"
    fi

    # Still serving: a gateway that survived but stopped accepting would
    # otherwise pass every check above.
    body=$(curl --silent --show-error --insecure --connect-timeout 2 --max-time 5 "https://127.0.0.1:$HTTP_PORT/" 2>&1) || true
    [ "$body" = "ok" ] || fail "round $round: the pool stopped serving, got: $body"

    round=$((round + 1))
done

info "$ROUNDS rounds, $((ROUNDS * BURST)) abandoned connections, no gateway died"

kill -TERM "$MASTER_PID"
wait "$MASTER_PID" 2>/dev/null || true
MASTER_PID=""

info "PASS"
