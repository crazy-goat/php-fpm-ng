#!/bin/sh
# Verifies task 042: a TLS pool may expose a redirect-only plain HTTP
# companion, while reserving ACME HTTP-01 requests for a local response.
set -eu

fail() {
    printf 'test-http-plain-listener.sh: FAIL: %s\n' "$*" >&2
    exit 1
}

[ "$#" -eq 1 ] || fail "usage: $0 /path/to/php-fpm-ng"
FPMNG_BIN=$1
[ -x "$FPMNG_BIN" ] || fail "not executable: $FPMNG_BIN"
command -v curl >/dev/null 2>&1 || fail "curl is required"
command -v openssl >/dev/null 2>&1 || fail "openssl is required"

DIR=$(mktemp -d)
MASTER_PID=
trap 'set +e; [ -n "$MASTER_PID" ] && kill -TERM "$MASTER_PID" 2>/dev/null; rm -rf "$DIR"' EXIT INT TERM
mkdir "$DIR/docroot"
cat > "$DIR/docroot/index.php" <<'EOF'
<?php echo "worker-content"; ?>
EOF
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost \
    -keyout "$DIR/key.pem" -out "$DIR/cert.pem" >/dev/null 2>&1

run_scenario() {
    reuseport=$1 offset=$2
    tls_port=$((19420 + offset))
    plain_port=$((19520 + offset))
    fcgi_port=$((19620 + offset))
    cat > "$DIR/fpm.conf" <<EOF
[global]
daemonize = no
error_log = $DIR/error.log
[gw]
pool.type = gateway
listen = 127.0.0.1:$tls_port
chdir = $DIR/docroot
http.gateways = 3
http.reuseport = $reuseport
http.plain_listen = 127.0.0.1:$plain_port
http.tls_cert = $DIR/cert.pem
http.tls_key = $DIR/key.pem
http.route[www] = /

[www]
pool.type = fastcgi
chdir = $DIR/docroot
listen = 127.0.0.1:$fcgi_port
pm = static
pm.max_children = 3
EOF
    "$FPMNG_BIN" -n -R -F -y "$DIR/fpm.conf" >"$DIR/stdout.log" 2>&1 &
    MASTER_PID=$!
    i=0
    while ! curl -sk --connect-timeout 1 "https://127.0.0.1:$tls_port/" >/dev/null 2>&1; do
        kill -0 "$MASTER_PID" 2>/dev/null || fail "master exited during startup"
        [ "$i" -lt 100 ] || fail "TLS listener did not start"
        i=$((i + 1))
        sleep 0.1
    done

    [ "$(curl -sk "https://127.0.0.1:$tls_port/")" = "worker-content" ] ||
        fail "TLS listener did not dispatch to a worker"
    headers=$(curl -sS -o "$DIR/body" -D - -H 'Host: example.test' \
        "http://127.0.0.1:$plain_port/a/b?x=1")
    printf '%s\n' "$headers" | grep -q '^HTTP/1.1 308 ' || fail "plain request was not redirected"
    printf '%s\n' "$headers" | grep -qi '^Location: https://example.test/a/b?x=1' ||
        fail "redirect did not preserve host, path, and query"
    [ ! -s "$DIR/body" ] || fail "redirect unexpectedly returned application content"

    code=$(curl -sS -o "$DIR/body" -w '%{http_code}' -H 'Host: example.test' \
        "http://127.0.0.1:$plain_port/.well-known/acme-challenge/token")
    [ "$code" = 404 ] || fail "unprovisioned ACME challenge returned $code, want 404"
    ! grep -q 'worker-content' "$DIR/body" || fail "ACME request reached application content"

    kill -TERM "$MASTER_PID"
    wait "$MASTER_PID" 2>/dev/null || true
    MASTER_PID=
}

run_scenario no 1
run_scenario yes 2
printf 'test-http-plain-listener.sh: all scenarios PASS\n'
