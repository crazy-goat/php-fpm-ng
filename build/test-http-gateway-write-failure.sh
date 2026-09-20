#!/bin/sh
# Issue #129: a synchronous hard write error while handing a request to an
# upstream must not free the connection or the upstream underneath a caller
# that still dereferences it.
#
# What it reproduced, measured on 192.168.8.50 (php-8.5.9, 2026-09-10), before
# the fix: valgrind reported two errors in the gateway process, an "Invalid
# read of size 8" 72 bytes into the freed fpm_http_conn and an "Invalid write
# of size 8" in smart_str_free_ex() 80 bytes into it -- the second
# smart_str_free(&c->out) in fpm_http_pump(), on a connection that
# fpm_http_upstream_fail() had already freed on the way out of the write. Both
# are silent without a trapping build: the freed zend_string's refcount word
# holds a heap pointer by then, so the release does not even reach free() and
# glibc reports nothing. Hence valgrind here rather than an assertion on the
# gateway staying alive -- it does stay alive, with a corrupted heap.
#
# The trigger is http.fault_upstream_write = N, a test-only pool directive
# read in fpm_http_upstream_flush(): it fails the Nth write() towards the pool with
# ECONNRESET without touching the socket. Nothing outside the process can time
# a hard error onto the write itself rather than onto the following read.
# A UNIX-socket upstream is part of the trigger too: connect() completes
# immediately, so the first write happens inside fpm_http_pump().
#
# Two rounds, the two shapes the failure has:
#   N=1  the write of a request to a connection that was just opened
#   N=2  the write of a request to an idle connection kept from request 1
#
# USE_ZEND_ALLOC=0 routes emalloc/efree to malloc/free so memcheck sees the
# smart_str allocations at all.
#
# No root needed: a loopback TCP port, a UNIX socket and files this script
# creates.
#
# Usage:
#   build/test-http-gateway-write-failure.sh /path/to/php-fpm-ng
set -eu

usage() {
    cat >&2 <<'EOF'
Usage: build/test-http-gateway-write-failure.sh /path/to/php-fpm-ng
EOF
    exit 2
}

fail() {
    printf 'test-http-gateway-write-failure.sh: FAIL: %s\n' "$*" >&2
    exit 1
}

info() {
    printf 'test-http-gateway-write-failure.sh: %s\n' "$*"
}

[ "$#" -eq 1 ] || usage
FPMNG_BIN=$1
[ -x "$FPMNG_BIN" ] || fail "not an executable file: $FPMNG_BIN"
case $FPMNG_BIN in
    /*) ;;
    *) FPMNG_BIN=$(pwd)/$FPMNG_BIN ;;
esac

command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v valgrind >/dev/null 2>&1 || fail "valgrind is required"

DIR=$(mktemp -d)
MASTER_PID=""
trap 'cleanup_all' EXIT INT TERM

cleanup_all() {
    set +e
    [ -n "$MASTER_PID" ] && kill -TERM "$MASTER_PID" >/dev/null 2>&1
    sleep 1
    [ -n "$MASTER_PID" ] && kill -KILL "$MASTER_PID" >/dev/null 2>&1
    rm -rf "$DIR"
}

HTTP_PORT=${FPMNG_HTTP_PORT:-19470}

mkdir -p "$DIR/docroot"
printf '%s' '<?php echo "served";' > "$DIR/docroot/served.php"
# The readiness probe below asks for this file. It has to be a static file:
# http.static answers it inside the gateway, without a worker and without a
# write to an upstream, so the injected failure is still waiting for the first
# real request. Any PHP path -- "/" included -- would spend it on the probe.
printf 'ready' > "$DIR/docroot/ready.txt"

write_config() {
    cat > "$DIR/fpm.conf" <<EOF
[global]
error_log = $DIR/error.log
pid = $DIR/fpm.pid
daemonize = no
[gw]
pool.type = gateway
listen = 127.0.0.1:$HTTP_PORT
chdir = $DIR/docroot
http.static = 1
http.gateways = 1
http.fault_upstream_write = $1
http.route[app] = /

[app]
pool.type = fastcgi
listen = $DIR/fcgi.sock
pm = static
pm.max_children = 2
chdir = $DIR/docroot
EOF
}

# Confirm the binary under test is the one this script names, before measuring
# anything with it (workflow.md, "Evidence").
strings "$FPMNG_BIN" | grep -q http.fault_upstream_write \
    || fail "$FPMNG_BIN has no http.fault_upstream_write directive: not an fpmng binary, or older than issue #129"

http_status() {
    curl -s -m 20 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$HTTP_PORT/served.php" || printf 'no-answer'
}

http_body() {
    curl -s -m 20 "http://127.0.0.1:$HTTP_PORT/served.php" || printf 'no-answer'
}

# One round: start the gateway under memcheck with the Nth write failing, drive
# three requests, stop it, and report what valgrind saw in OUR code.
round() {
    fail_at=$1
    expected_first=$2
    expected_second=$3

    rm -f "$DIR"/vg.*.log "$DIR/error.log" "$DIR/fpm.pid"
    write_config "$fail_at"
    USE_ZEND_ALLOC=0 valgrind \
        --trace-children=yes --log-file="$DIR/vg.%p.log" \
        "$FPMNG_BIN" -n -y "$DIR/fpm.conf" -F > "$DIR/fpm.out" 2>&1 &
    MASTER_PID=$!

    # Startup under memcheck is slow (measured: ~20 s on the poligon), and the
    # request whose write is made to fail must not be the readiness probe.
    waited=0
    while [ ! -S "$DIR/fcgi.sock" ] \
        || [ "$(curl -s -m 5 "http://127.0.0.1:$HTTP_PORT/ready.txt" 2>/dev/null || true)" != "ready" ]; do
        waited=$((waited + 1))
        [ "$waited" -lt 180 ] || fail "the gateway did not answer within 180 s (fail_at=$fail_at)"
        sleep 1
        kill -0 "$MASTER_PID" 2>/dev/null || fail "the master exited during startup (fail_at=$fail_at)"
    done
    # The probe above was answered from the document root by the gateway
    # itself, so no write towards the pool has been spent yet: the requests
    # below are the first ones this process writes.

    first=$(http_status)
    [ "$first" = "$expected_first" ] \
        || fail "fail_at=$fail_at: request 1 answered $first, expected $expected_first"
    second=$(http_status)
    [ "$second" = "$expected_second" ] \
        || fail "fail_at=$fail_at: request 2 answered $second, expected $expected_second"
    third=$(http_body)
    [ "$third" = "served" ] \
        || fail "fail_at=$fail_at: request 3 was not served, got '$third'"

    kill -TERM "$MASTER_PID" >/dev/null 2>&1
    # memcheck writes its report at exit; give every traced process time for it.
    waited=0
    while kill -0 "$MASTER_PID" 2>/dev/null; do
        waited=$((waited + 1))
        [ "$waited" -lt 60 ] || break
        sleep 1
    done
    MASTER_PID=""
    sleep 3

    if grep -A 25 -E '== Invalid (read|write|free)|== Mismatched free' "$DIR"/vg.*.log 2>/dev/null \
        | grep -q 'fpm_http'; then
        info "valgrind reported a memory error in our code (fail_at=$fail_at):"
        grep -A 25 -E '== Invalid (read|write|free)|== Mismatched free' "$DIR"/vg.*.log >&2
        fail "fail_at=$fail_at: invalid access in fpm_http.c under a synchronous write failure"
    fi

    # A gateway that died would be respawned and the requests above could still
    # have been served by its replacement, so the log is checked as well.
    if grep -qE 'http gateway [0-9]+ \(pid [0-9]+\) (killed by signal|exited with code)' "$DIR/error.log"; then
        grep -E 'http gateway' "$DIR/error.log" >&2
        fail "fail_at=$fail_at: a gateway process died"
    fi

    grep -q "http: upstream .*Connection reset by peer" "$DIR/error.log" \
        || fail "fail_at=$fail_at: the injected write failure was not reported in the log"

    info "fail_at=$fail_at: request 1 $first, request 2 $second, request 3 served; no invalid access in fpm_http.c"
}

# fail_at=1: the write to a connection opened for this very request.
round 1 502 200
# fail_at=2: request 1 succeeds and its connection stays idle, so request 2 is
# the one written to a reused upstream.
round 2 200 502

info "PASS"
