#!/bin/sh
# Verifies issue #172: with http.tls_wait_for_cert, an http pool starts before
# its certificate exists (NO_CERT), refuses connections on the TLS port while
# it waits, answers only ACME HTTP-01 on http.plain_listen, and opens the TLS
# listener in EVERY gateway process once the certificate is installed -- all
# without a restart, and without weakening anything when the opt-in is absent.
#
# Why a shell harness and not a .phpt: the assertions are about listening
# sockets, TLS handshakes and three separate gateway processes, none of which
# a .phpt can see. Same reasoning, and the same shape, as
# build/test-http-tls-reload.sh, which this is deliberately modelled on.
#
# No root needed: only TCP ports above 19000 and PEM files this script creates.
#
# Usage:
#   build/test-http-tls-wait-for-cert.sh /path/to/php-fpm-ng
set -eu

usage() {
    cat >&2 <<'EOF'
Usage: build/test-http-tls-wait-for-cert.sh /path/to/php-fpm-ng
EOF
    exit 2
}

fail() {
    printf 'test-http-tls-wait-for-cert.sh: FAIL: %s\n' "$*" >&2
    [ -f "${DIR:-}/error.log" ] && sed 's/^/    log| /' "$DIR/error.log" >&2
    exit 1
}

info() {
    printf 'test-http-tls-wait-for-cert.sh: %s\n' "$*"
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
MASTER_PID=""
trap 'cleanup_all' EXIT INT TERM

cleanup_all() {
    set +e
    [ -n "$MASTER_PID" ] && kill -TERM "$MASTER_PID" >/dev/null 2>&1
    sleep 0.3
    [ -n "$MASTER_PID" ] && kill -KILL "$MASTER_PID" >/dev/null 2>&1
    rm -rf "$DIR"
}

# Deadline for a transition to have reached every gateway process. Generous on
# purpose, exactly as in test-http-tls-reload.sh: what the script can know is
# that a healthy transition lands *eventually*, never that it lands within any
# particular number of seconds on any particular machine.
DEADLINE=${FPMNG_WAIT_FOR_CERT_DEADLINE:-30}

wait_for_log_count() {
    pattern=$1 want=$2 label=$3
    deadline=$(($(date +%s) + DEADLINE))
    while :; do
        now=$(grep -c "$pattern" "$DIR/error.log" 2>/dev/null || true)
        if [ "${now:-0}" -ge "$want" ]; then
            return 0
        fi
        if [ "$(date +%s)" -ge "$deadline" ]; then
            fail "$label: waited ${DEADLINE}s for $want log line(s) matching '$pattern', saw ${now:-0}"
        fi
        kill -0 "$MASTER_PID" 2>/dev/null || fail "$label: the master exited while waiting"
        sleep 0.2
    done
}

# curl's exit code, not its HTTP status: the difference between "refused the
# connection" (7) and "spoke TLS and failed" (35) is the whole of criterion 3.
# `|| rc=$?` rather than a bare call: a non-zero curl is the expected result
# here, and under `set -e` a bare call would abort the script at exactly the
# assertion it exists to make.
curl_exit() {
    rc=0
    curl --silent --output /dev/null --insecure --connect-timeout 2 --max-time 5 "$1" >/dev/null 2>&1 || rc=$?
    echo "$rc"
}

http_code() {
    curl --silent --output /dev/null --insecure --connect-timeout 2 --max-time 5 -w '%{http_code}' "$1" 2>/dev/null || true
}

HTTP_PORT=19360
PLAIN_PORT=19361
FCGI_PORT=19362
GATEWAYS=3

info "generating a leaf certificate, NOT yet installed"
mkdir -p "$DIR/certs" "$DIR/pending" "$DIR/docroot"
openssl genrsa -out "$DIR/pending/key.pem" 2048 >/dev/null 2>&1
openssl req -x509 -new -key "$DIR/pending/key.pem" -sha256 -days 2 \
    -subj /CN=waitforcert.test -out "$DIR/pending/cert.pem" >/dev/null 2>&1
SERIAL=$(openssl x509 -in "$DIR/pending/cert.pem" -noout -serial | sed 's/^serial=//')

cat > "$DIR/docroot/index.php" <<'EOF'
<?php echo "ok"; ?>
EOF

write_conf() {
    optin=$1 out=$2 reuseport=${3:-no}
    cat > "$out" <<EOF
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
http.gateways = $GATEWAYS
http.reuseport = $reuseport
http.listen = 127.0.0.1:$HTTP_PORT
http.plain_listen = 127.0.0.1:$PLAIN_PORT
http.front_controller = /index.php
http.tls_cert = $DIR/certs/fullchain.pem
http.tls_key = $DIR/certs/privkey.pem
http.tls_reload_check = 1
http.tls_wait_for_cert = $optin
EOF
}

# --- criterion 1: without the opt-in, nothing changes -----------------------
info "criterion 1: the same config with http.tls_wait_for_cert = no must not start"
write_conf no "$DIR/fpm-noopt.conf"
if "$FPMNG_BIN" -n -R -F -y "$DIR/fpm-noopt.conf" > "$DIR/noopt.log" 2>&1; then
    fail "criterion 1: php-fpm-ng started with a missing certificate and no opt-in"
fi
grep -q "cannot read" "$DIR/noopt.log" ||
    fail "criterion 1: startup failed, but not with the 'cannot read' certificate error: $(cat "$DIR/noopt.log")"
if grep -qi "falling back to plain" "$DIR/noopt.log"; then
    fail "criterion 1: the startup failure mentions a plain-HTTP fallback, which must never happen"
fi
info "criterion 1: ok -- a missing certificate is still a hard startup failure"

# A certificate that EXISTS but does not parse must still fail, opt-in or not:
# that is an operator error, not an unfinished issuance.
info "criterion 1: an unparseable certificate must fail even WITH the opt-in"
printf 'not a certificate\n' > "$DIR/certs/fullchain.pem"
cp "$DIR/pending/key.pem" "$DIR/certs/privkey.pem"
write_conf yes "$DIR/fpm-broken.conf"
if "$FPMNG_BIN" -n -R -F -y "$DIR/fpm-broken.conf" > "$DIR/broken.log" 2>&1; then
    fail "criterion 1: php-fpm-ng started with an unparseable certificate and the opt-in set"
fi
info "criterion 1: ok -- http.tls_wait_for_cert does not excuse a broken certificate"
rm -f "$DIR/certs/fullchain.pem" "$DIR/certs/privkey.pem"

# SNI certificates are loaded once at startup and are never part of the
# certificate-watch poll, so a pool that starts in NO_CERT would come out of
# the transition serving the primary certificate for every SNI name -- and
# silently, because the validation that would have complained was skipped with
# the rest of the startup load. The combination has to be refused at config
# time; this asserts it is, and that the operator is told which two directives
# conflict rather than just that startup failed.
info "criterion 1: http.tls_wait_for_cert must be refused together with http.tls_sni_cert"
write_conf yes "$DIR/fpm-sni.conf"
printf 'http.tls_sni_cert = example.test:%s\n' "$DIR/pending/cert.pem" >> "$DIR/fpm-sni.conf"
if "$FPMNG_BIN" -n -R -F -y "$DIR/fpm-sni.conf" > "$DIR/sni.log" 2>&1; then
    fail "criterion 1: php-fpm-ng started with http.tls_wait_for_cert and http.tls_sni_cert together"
fi
grep -q "tls_sni_cert" "$DIR/sni.log" ||
    fail "criterion 1: startup failed, but the message does not name http.tls_sni_cert: $(cat "$DIR/sni.log")"
info "criterion 1: ok -- the SNI combination is refused, and the message says so"

# --- NO_CERT ----------------------------------------------------------------
write_conf yes "$DIR/fpm.conf"
info "starting php-fpm-ng in NO_CERT ($GATEWAYS gateways, http.tls_reload_check=1)"
: > "$DIR/error.log"
"$FPMNG_BIN" -n -R -F -y "$DIR/fpm.conf" > "$DIR/stdout.log" 2>&1 &
MASTER_PID=$!

wait_for_log_count "NO_CERT -- no certificate" 1 "startup"
wait_for_log_count "ready to handle connections" 1 "startup"

# The error log must be clean: a first boot is the configured state, not a
# fault, and an ERROR line here would train an operator to ignore the log.
if grep -q "ERROR" "$DIR/error.log"; then
    fail "NO_CERT: the error log contains an ERROR line on a first boot"
fi

info "criterion 3: the TLS port must refuse the connection, not fail a handshake"
rc=$(curl_exit "https://127.0.0.1:$HTTP_PORT/")
[ "$rc" = "7" ] ||
    fail "criterion 3: curl exited $rc connecting to the TLS port, want 7 (could not connect). 35 would mean a handshake was attempted, i.e. the listener is up"
info "criterion 3: ok -- curl exit 7 (connection refused)"

info "criterion 2: the plain listener answers ACME only"
code=$(http_code "http://127.0.0.1:$PLAIN_PORT/")
[ "$code" = "503" ] ||
    fail "criterion 2: / on the plain listener returned $code, want 503. 308 would redirect the client to a port that is refusing connections"
code=$(http_code "http://127.0.0.1:$PLAIN_PORT/.well-known/acme-challenge/not-provisioned")
[ "$code" = "404" ] ||
    fail "criterion 2: an unprovisioned ACME challenge returned $code, want 404 -- the challenge namespace must be answered by the challenge responder, not by the 503 above"
info "criterion 2: ok -- / is 503, the ACME namespace is 404 from the responder"

# --- NO_CERT -> READY -------------------------------------------------------
info "installing the certificate (key first, then the chain, as the ACME client does)"
cp "$DIR/pending/key.pem" "$DIR/certs/privkey.pem"
chmod 600 "$DIR/certs/privkey.pem"
cp "$DIR/pending/cert.pem" "$DIR/certs/fullchain.pem"

wait_for_log_count "TLS certificate found on disk" 1 "transition (master)"

# Criterion 4, and the reason this is a log COUNT rather than a burst of
# requests: the notice is emitted once per gateway process, so the count is
# exact. A mechanism that reached only the first-forked child leaves it at 1
# of 3 and fails here deterministically. Sampling the wire instead could not
# distinguish "all three opened" from "one opened and served everything".
info "criterion 4: every one of the $GATEWAYS gateways must leave NO_CERT"
wait_for_log_count "leaving NO_CERT" "$GATEWAYS" "transition (gateways)"
info "criterion 4: ok -- $GATEWAYS gateway processes each logged the transition"

info "the TLS port now serves the installed certificate"
i=0
while [ "$i" -lt 8 ]; do
    got=$(echo | openssl s_client -connect "127.0.0.1:$HTTP_PORT" 2>/dev/null |
        openssl x509 -noout -serial 2>/dev/null | sed 's/^serial=//')
    [ "$got" = "$SERIAL" ] || fail "post-transition: connection $i served serial '$got', want $SERIAL"
    i=$((i + 1))
done
body=$(curl --silent --show-error --insecure --connect-timeout 2 --max-time 5 "https://127.0.0.1:$HTTP_PORT/")
[ "$body" = "ok" ] || fail "post-transition: expected body 'ok' over TLS, got: $body"

info "the plain listener goes back to redirecting"
code=$(http_code "http://127.0.0.1:$PLAIN_PORT/")
[ "$code" = "308" ] ||
    fail "post-transition: / on the plain listener returned $code, want 308 -- the TLS port is open now, so the redirect must come back"

# --- criterion 5: READY is one-way ------------------------------------------
info "criterion 5: deleting the certificate must NOT take TLS down"
rm -f "$DIR/certs/fullchain.pem" "$DIR/certs/privkey.pem"
wait_for_log_count "has become unreadable" 1 "criterion 5"
body=$(curl --silent --show-error --insecure --connect-timeout 2 --max-time 5 "https://127.0.0.1:$HTTP_PORT/")
[ "$body" = "ok" ] || fail "criterion 5: TLS stopped serving after the certificate file was deleted, got: $body"
code=$(http_code "http://127.0.0.1:$PLAIN_PORT/")
[ "$code" = "308" ] ||
    fail "criterion 5: the plain listener fell back to $code after the deletion -- READY must be one-way"
info "criterion 5: ok -- still serving, and the operator got one warning"

# And it recovers by itself when the pair comes back, without a second warning
# storm: the notice fires once, on the edge.
cp "$DIR/pending/key.pem" "$DIR/certs/privkey.pem"
cp "$DIR/pending/cert.pem" "$DIR/certs/fullchain.pem"
wait_for_log_count "is readable again" 1 "criterion 5 recovery"
warnings=$(grep -c "has become unreadable" "$DIR/error.log")
[ "$warnings" = "1" ] ||
    fail "criterion 5: the 'unreadable' warning was logged $warnings times, want exactly 1 -- it must fire on the edge, not every tick"
info "criterion 5: ok -- recovery is announced, and the warning fired exactly once"

# --- the same transition with http.reuseport ---------------------------------
# A separate phase because it is a genuinely different code path: with
# http.reuseport each gateway CHILD binds its own socket after the fork
# (fpm_http.c, the gw->reuseport branch in the child), so the bind-without-
# listen and the later listen() both happen per process and inside a
# SO_REUSEPORT group, rather than once in the master on an inherited fd.
# Kept short -- the criteria above are already covered; what this adds is that
# the second binding path reaches the same two states.
info "http.reuseport: the same NO_CERT -> READY transition on the per-child binding path"
kill -TERM "$MASTER_PID" >/dev/null 2>&1 || true
wait "$MASTER_PID" 2>/dev/null || true
MASTER_PID=""
rm -f "$DIR/certs/fullchain.pem" "$DIR/certs/privkey.pem"
: > "$DIR/error.log"

write_conf yes "$DIR/fpm-reuseport.conf" yes
"$FPMNG_BIN" -n -R -F -y "$DIR/fpm-reuseport.conf" > "$DIR/stdout-reuseport.log" 2>&1 &
MASTER_PID=$!
wait_for_log_count "ready to handle connections" 1 "reuseport startup"

rc=$(curl_exit "https://127.0.0.1:$HTTP_PORT/")
[ "$rc" = "7" ] ||
    fail "http.reuseport: curl exited $rc in NO_CERT, want 7 -- each child must bind without listening too"

cp "$DIR/pending/key.pem" "$DIR/certs/privkey.pem"
cp "$DIR/pending/cert.pem" "$DIR/certs/fullchain.pem"
wait_for_log_count "leaving NO_CERT" "$GATEWAYS" "reuseport transition"
body=$(curl --silent --show-error --insecure --connect-timeout 2 --max-time 5 "https://127.0.0.1:$HTTP_PORT/")
[ "$body" = "ok" ] || fail "http.reuseport: expected body 'ok' over TLS after the transition, got: $body"
info "http.reuseport: ok -- $GATEWAYS gateways, refused in NO_CERT, serving after"

info "PASS"
