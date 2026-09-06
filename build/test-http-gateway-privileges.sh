#!/bin/sh
# Verifies task 010: the HTTP gateway process drops root before it ever
# accepts a connection, instead of running with the master's identity for its
# whole life (see tasks/010-http-gateway-drop-privileges.md).
#
# This inspects the *running* gateway process's uid/gid under /proc, not the
# source -- acceptance criterion 6 of that task asks for exactly that. It must
# run as root: only a root master ever has privileges to drop in the first
# place, and only root can start it for this test.
#
# Usage:
#   sudo build/test-http-gateway-privileges.sh /path/to/php-fpm-ng
#
# Three scenarios, each its own pool/ports/log directory:
#   plain      -- http.reuseport off, the ordinary case.
#   reuseport  -- http.reuseport on: the child does its own bind() after
#                 fork(), which is the ordering hazard the task's notes call
#                 out explicitly.
#   root-pool  -- pool has no user/group at all (only legal under
#                 --allow-to-run-as-root): the gateway is expected to *stay*
#                 root and say so in the log, not drop silently.
set -eu

usage() {
    cat >&2 <<'EOF'
Usage: sudo build/test-http-gateway-privileges.sh /path/to/php-fpm-ng
EOF
    exit 2
}

fail() {
    printf 'test-http-gateway-privileges.sh: FAIL: %s\n' "$*" >&2
    exit 1
}

info() {
    printf 'test-http-gateway-privileges.sh: %s\n' "$*"
}

[ "$#" -eq 1 ] || usage
FPMNG_BIN=$1
[ -x "$FPMNG_BIN" ] || fail "not an executable file: $FPMNG_BIN"
case $FPMNG_BIN in
    /*) ;;
    *) FPMNG_BIN=$(pwd)/$FPMNG_BIN ;;
esac

[ "$(id -u)" -eq 0 ] || fail "must run as root (only a root master has anything to drop)"
command -v curl >/dev/null 2>&1 || fail "curl is required"

NOBODY_UID=$(id -u nobody) || fail "no 'nobody' user on this system"
NOBODY_GID=$(id -g nobody)
NOBODY_GROUP=$(id -gn nobody)

RUN_ROOT=$(mktemp -d)
# mktemp -d defaults to 0700: the dropped-to worker (running as $NOBODY_UID,
# unrelated to this task -- that drop already existed) needs to traverse this
# directory to reach docroot/index.php, or every request 404s no matter how
# correctly the gateway itself dropped privileges.
chmod 755 "$RUN_ROOT"
trap 'cleanup_all' EXIT INT TERM
MASTER_PIDS=""

cleanup_all() {
    set +e
    for pid in $MASTER_PIDS; do
        kill -TERM "$pid" >/dev/null 2>&1
    done
    sleep 0.3
    for pid in $MASTER_PIDS; do
        kill -KILL "$pid" >/dev/null 2>&1
    done
    rm -rf "$RUN_ROOT"
}

# Every /proc/<pid>/status Uid:/Gid: line has four columns: real, effective,
# saved-set, filesystem. All four must equal $2 for the drop (or the lack of
# one) to actually hold, not just the one a lazier check would read.
assert_id_line() {
    pid=$1 want=$2 label=$3 file=$4
    line=$(grep "^$label:" "/proc/$pid/status") || fail "/proc/$pid/status has no $label: line (did the process exit?)"
    for col in 2 3 4 5; do
        got=$(printf '%s' "$line" | awk -v c="$col" '{print $c}')
        [ "$got" = "$want" ] || fail "$file: pid $pid $label column $col is $got, want $want ($line)"
    done
}

wait_for_port() {
    port=$1 master_pid=$2
    i=0
    while [ "$i" -lt 100 ]; do
        if curl --silent --output /dev/null --connect-timeout 1 "http://127.0.0.1:$port/" >/dev/null 2>&1; then
            return 0
        fi
        # curl's own exit status can't tell "connection refused" (still
        # starting) from "no one is listening anymore" (master died) apart --
        # this does, and turns a silent 100x0.1s timeout into a fast failure.
        kill -0 "$master_pid" 2>/dev/null || return 1
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

# find_gateway_pids POOL -- one pid per "http gateway POOL [N]" process,
# newline-separated. Matches fpm_env_setproctitle()'s title in fpm_http.c.
find_gateway_pids() {
    pool=$1
    pgrep -f "http gateway $pool \[" || true
}

run_scenario() {
    name=$1 reuseport=$2 pool_has_user=$3
    dir="$RUN_ROOT/$name"
    mkdir -p "$dir/docroot"
    cat > "$dir/docroot/index.php" <<'EOF'
<?php echo "ok"; ?>
EOF
    # $dir itself needs to be writable by the gateway's dropped-to identity
    # (nobody), or http.access_log can never be created there; 755 alone
    # covers the read/traverse the worker needs for docroot but not this.
    chmod 777 "$dir"
    chmod 755 "$dir/docroot"
    chmod 644 "$dir/docroot/index.php"

    fcgi_port=$((19100 + $4))
    http_port=$((19200 + $4))

    {
        echo "[global]"
        echo "daemonize = no"
        echo "error_log = $dir/error.log"
        echo "pid = $dir/fpm.pid"
        echo "log_level = notice"
        echo "[www]"
        if [ "$pool_has_user" = "yes" ]; then
            echo "user = nobody"
            echo "group = $NOBODY_GROUP"
        fi
        echo "chdir = $dir/docroot"
        echo "listen = 127.0.0.1:$fcgi_port"
        echo "pm = static"
        echo "pm.max_children = 1"
        echo "pool.type = http"
        echo "http.gateways = 1"
        echo "http.reuseport = $reuseport"
        echo "http.listen = 127.0.0.1:$http_port"
        echo "http.front_controller = /index.php"
        echo "http.access_log = $dir/access.log"
    } > "$dir/fpm.conf"

    extra_opt=""
    [ "$pool_has_user" = "yes" ] || extra_opt="-R"

    info "[$name] starting php-fpm-ng (reuseport=$reuseport, pool user=$pool_has_user)"
    "$FPMNG_BIN" -n $extra_opt -F -y "$dir/fpm.conf" > "$dir/stdout.log" 2>&1 &
    master_pid=$!
    MASTER_PIDS="$MASTER_PIDS $master_pid"

    wait_for_port "$http_port" "$master_pid" || {
        cat "$dir/error.log" "$dir/stdout.log" 2>/dev/null >&2
        fail "[$name] gateway never accepted a connection on 127.0.0.1:$http_port"
    }

    # Positive control: the master itself must still be root -- this whole
    # task is about the master staying privileged while only the network-
    # facing gateway drops, so a master that also dropped would make the
    # gateway assertions below meaningless.
    assert_id_line "$master_pid" 0 Uid "$name"

    gw_pids=$(find_gateway_pids www)
    [ -n "$gw_pids" ] || fail "[$name] no 'http gateway www [...]' process found"

    for gw_pid in $gw_pids; do
        if [ "$pool_has_user" = "yes" ]; then
            assert_id_line "$gw_pid" "$NOBODY_UID" Uid "$name"
            assert_id_line "$gw_pid" "$NOBODY_GID" Gid "$name"
        else
            # root-pool scenario: nothing to drop to, gateway stays root --
            # and says so, per acceptance criterion 5 (never silent).
            assert_id_line "$gw_pid" 0 Uid "$name"
            grep -q "gateway keeps running as root" "$dir/error.log" \
                || fail "[$name] gateway stayed root without logging why"
        fi
    done

    # Functional check: the gateway must still actually work post-drop --
    # accept the connection, serve the request, write the access log --
    # not just exist with the right uid. front_controller runs index.php.
    body=$(curl --silent --show-error --connect-timeout 2 --max-time 5 "http://127.0.0.1:$http_port/")
    if [ "$body" != "ok" ]; then
        cat "$dir/error.log" "$dir/stdout.log" 2>/dev/null >&2
        fail "[$name] expected body 'ok', got: $body"
    fi

    [ -f "$dir/access.log" ] || fail "[$name] http.access_log was never created"
    if [ "$pool_has_user" = "yes" ]; then
        log_uid=$(stat -c '%u' "$dir/access.log" 2>/dev/null || stat -f '%u' "$dir/access.log")
        [ "$log_uid" = "$NOBODY_UID" ] || fail "[$name] access.log owned by uid $log_uid, want $NOBODY_UID (opened before the drop?)"
    fi

    kill -TERM "$master_pid"
    wait "$master_pid" 2>/dev/null || true
    MASTER_PIDS=$(printf '%s\n' $MASTER_PIDS | grep -v "^$master_pid\$" || true)

    info "[$name] PASS"
}

run_scenario plain     no  yes 1
run_scenario reuseport yes yes 2
run_scenario root-pool no  no  3

info "all scenarios PASS"
