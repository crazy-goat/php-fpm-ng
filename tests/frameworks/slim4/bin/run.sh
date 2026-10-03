#!/usr/bin/env bash
# Slim 4 smoke test for the executors main ships (issue #602).
#
#   FPMNG=/path/to/php-fpm-ng [PHP=/path/to/php] tests/frameworks/slim4/bin/run.sh
#
# Starts php-fpm-ng with the stock Slim 4 entry script in each of these setups and runs
# bin/run.php against it:
#
#   gateway-fastcgi  pool.type = gateway in front of pool.type = fastcgi (classic executor)
#   http-direct      pool.type = http-direct, pool.executor = classic
#
# Each setup runs twice: plain, and with PHP-DI plus Slim's route cache, so that every row of
# run.php is measured at least once. Setting SLIM_CONTAINER (none|php-di) or SLIM_ROUTE_CACHE (0|1)
# runs only that one combination. MODES selects the setups (space-separated).
#
# No MySQL, no Redis, no Docker: Composer and a PHP CLI with cURL are the only prerequisites.
# Exits 0 only when every run passed, 2 when a prerequisite is missing.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
RUN_DIR=${RUN_DIR:-$ROOT/.run}
PHP=${PHP:-php}
FPMNG=${FPMNG:-php-fpm-ng}
MODES=${MODES:-gateway-fastcgi http-direct}
HTTP_PORT=${HTTP_PORT:-22626}
FCGI_PORT=${FCGI_PORT:-22625}

for command in curl "$PHP" "$FPMNG"; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "$command is required" >&2
        exit 2
    fi
done

if [ ! -f "$ROOT/vendor/autoload.php" ]; then
    PHP="$PHP" RUN_DIR="$RUN_DIR" "$ROOT/bin/provision.sh"
fi
if [ ! -f "$ROOT/vendor/autoload.php" ]; then
    echo "Slim 4 dependency provisioning did not create vendor/autoload.php" >&2
    exit 2
fi

port_is_free() {
    ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
}

# Bump past whatever is already bound instead of failing, so a busy machine needs no manual
# port bookkeeping.
choose_free_port() {
    local port=$1
    while ! port_is_free "$port"; do
        port=$((port + 1))
    done
    echo "$port"
}

stop_pool() {
    local pid_file=$1 pid
    if [ -f "$pid_file" ]; then
        pid=$(cat "$pid_file")
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
            for _ in $(seq 1 50); do
                kill -0 "$pid" 2>/dev/null || break
                sleep 0.1
            done
            kill -KILL "$pid" 2>/dev/null || true
        fi
    fi
}

CURRENT_PID_FILE=
trap 'stop_pool "$CURRENT_PID_FILE"' EXIT INT TERM

# http-direct runs one worker, so every request is served by the same process and state that
# leaks from one request to the next is visible to the probe. The gateway rejects a request
# with 503 when its pool has no free worker (fpm_http_reject_queued()), so the fastcgi pool gets
# one worker per parallel probe request. bin/run.php then sends sequential requests as well.
write_pools() {
    local mode=$1 dir=$2 http_port=$3 fcgi_port=$4 container=$5 route_cache=$6
    local app_env workers=1
    [ "$mode" = gateway-fastcgi ] && workers=8
    app_env="chdir = $ROOT/public
pm = static
pm.max_children = $workers
catch_workers_output = yes
php_admin_flag[opcache.enable] = off
env[SLIM_CONTAINER] = \"$container\"
env[SLIM_ROUTE_CACHE] = \"$route_cache\"
env[SLIM_ROUTE_CACHE_FILE] = \"$dir/route-cache.php\""
    {
        cat <<CONF
[global]
pid = $dir/php-fpm-ng.pid
error_log = $dir/php-fpm-ng.log
log_level = notice
daemonize = yes

CONF
        case "$mode" in
            gateway-fastcgi)
                cat <<CONF
[gw]
pool.type = gateway
listen = 127.0.0.1:$http_port
; The gateway builds SCRIPT_FILENAME from its own chdir and front controller, so both name
; the same public/ directory as the application pool.
chdir = $ROOT/public
http.front_controller = /index.php
; One gateway process, on purpose: with 2 processes a queued request is re-pumped only when an
; upstream is freed in the same process, so part of a burst of 8 parallel requests waited for
; http.pool_full_wait_ms and then got 503 although the 8 fastcgi workers were idle (3 of 10
; runs failed). A single process is deterministic.
http.gateways = 1
http.pool_full_policy = wait
http.pool_full_queue_max = 32
http.pool_full_wait_ms = 5000
http.access_log = $dir/http.access
http.route[www] = /

[www]
pool.type = fastcgi
listen = 127.0.0.1:$fcgi_port
$app_env
CONF
                ;;
            http-direct)
                cat <<CONF
[direct]
pool.type = http-direct
pool.executor = classic
listen = 127.0.0.1:$http_port
http.front_controller = /index.php
http.read_timeout = 10000
http.max_body = 1M
$app_env
CONF
                ;;
            *)
                echo "unsupported mode: $mode" >&2
                exit 2
                ;;
        esac
    } > "$dir/fpm.conf"
}

run_one() {
    local mode=$1 container=$2 route_cache=$3
    local dir="$RUN_DIR/$mode-$container-$route_cache"
    local fcgi_port http_port

    rm -rf "$dir"
    mkdir -p "$dir/sessions"
    fcgi_port=$(choose_free_port "$FCGI_PORT")
    http_port=$(choose_free_port "$HTTP_PORT")
    # Never the same port twice: the default pair is only one apart.
    if [ "$http_port" = "$fcgi_port" ]; then
        http_port=$(choose_free_port $((fcgi_port + 1)))
    fi

    cat > "$dir/php.ini" <<INI
display_errors=1
error_reporting=E_ALL
log_errors=1
error_log=$dir/php-errors.log
memory_limit=256M
date.timezone=UTC
session.save_handler=files
session.save_path=$dir/sessions
session.gc_probability=0
INI

    write_pools "$mode" "$dir" "$http_port" "$fcgi_port" "$container" "$route_cache"
    CURRENT_PID_FILE="$dir/php-fpm-ng.pid"

    echo "=== $mode, container=$container, route-cache=$route_cache (http $http_port) ==="
    "$FPMNG" -c "$dir/php.ini" -y "$dir/fpm.conf"

    local ready=0
    for _ in $(seq 1 100); do
        if curl --silent --fail "http://127.0.0.1:$http_port/health" > "$dir/health.json" 2>/dev/null \
            && grep -q '"ok":true' "$dir/health.json"; then
            ready=1
            break
        fi
        sleep 0.1
    done
    if [ "$ready" -ne 1 ]; then
        echo "php-fpm-ng did not become ready in mode $mode" >&2
        cat "$dir/php-fpm-ng.log" >&2 || true
        stop_pool "$CURRENT_PID_FILE"
        return 1
    fi

    local rc=0
    SLIM_BASE_URL="http://127.0.0.1:$http_port" \
    SLIM_CONTAINER="$container" \
    SLIM_ROUTE_CACHE="$route_cache" \
    SLIM_ROUTE_CACHE_FILE="$dir/route-cache.php" \
    "$PHP" "$ROOT/bin/run.php" || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "--- php-fpm-ng.log ($mode) ---" >&2
        cat "$dir/php-fpm-ng.log" >&2 || true
    fi
    stop_pool "$CURRENT_PID_FILE"
    return "$rc"
}

combos=()
if [ -n "${SLIM_CONTAINER:-}${SLIM_ROUTE_CACHE:-}" ]; then
    combos=("${SLIM_CONTAINER:-none} ${SLIM_ROUTE_CACHE:-0}")
else
    combos=("none 0" "php-di 1")
fi

failed=0
for mode in $MODES; do
    for combo in "${combos[@]}"; do
        # shellcheck disable=SC2086 # the combination is two words on purpose
        run_one "$mode" $combo || failed=1
    done
done

if [ "$failed" -ne 0 ]; then
    echo "Slim 4 smoke test FAILED" >&2
    exit 1
fi
echo "Slim 4 smoke test passed in: $MODES"
