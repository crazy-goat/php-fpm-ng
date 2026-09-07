#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
RUN_DIR=${RUN_DIR:-$ROOT/.run}
PHP=${PHP:-php}
FPMNG=${FPMNG:-php-fpm-ng}
HTTP_PORT=${HTTP_PORT:-22626}
FCGI_PORT=${FCGI_PORT:-22625}
SLIM_CONTAINER=${SLIM_CONTAINER:-none}
SLIM_ROUTE_CACHE=${SLIM_ROUTE_CACHE:-0}
SLIM_ROUTE_CACHE_FILE=${SLIM_ROUTE_CACHE_FILE:-$RUN_DIR/route-cache.php}
# SERVICE_MODE=docker (default) provisions a private MySQL/Redis via
# compose.yaml so the runner works on a clean machine with nothing
# pre-provisioned but Docker. SERVICE_MODE=external keeps the previous
# behaviour of pointing at already-running services via the SLIM_DB_*
# and SLIM_REDIS_* variables below (e.g. the shared test box).
SERVICE_MODE=${SERVICE_MODE:-docker}
MYSQL_PORT=${MYSQL_PORT:-13308}
REDIS_PORT=${REDIS_PORT:-16381}
MYSQL_ROOT_PASSWORD=${MYSQL_ROOT_PASSWORD:-fpmng-slim4-root}
DOCKER_STARTED=0
RUN_ID=${FPMNG_RUN_ID:-$(date -u +%Y%m%dt%H%M%Sz)_$$}

case "$SLIM_CONTAINER" in
    none|php-di) ;;
    *) echo "unsupported SLIM_CONTAINER: $SLIM_CONTAINER" >&2; exit 2 ;;
esac
case "$SLIM_ROUTE_CACHE" in
    0|1) ;;
    *) echo "SLIM_ROUTE_CACHE must be 0 or 1" >&2; exit 2 ;;
esac

for command in curl nc; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "$command is required (port checks and the HTTP client need it)" >&2
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

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

port_is_free() {
    ! nc -z 127.0.0.1 "$1" >/dev/null 2>&1
}

# Same pattern as tests/frameworks/symfony/run.sh: bump past whatever is
# already bound instead of failing, so parallel runs and a busy dev machine
# do not need manual port bookkeeping.
choose_free_port() {
    local port=$1
    while ! port_is_free "$port"; do
        port=$((port + 1))
    done
    echo "$port"
}

COMPOSE=()

cleanup_services() {
    if [ "$DOCKER_STARTED" -eq 1 ]; then
        # Never FLUSHALL/FLUSHDB: the whole private compose project (and its
        # volumes) is torn down instead, so no shared MySQL/Redis is touched.
        "${COMPOSE[@]}" down --volumes --remove-orphans >/dev/null 2>&1 || true
        DOCKER_STARTED=0
    fi
}

setup_services() {
    if [ "$SERVICE_MODE" != docker ] && [ "$SERVICE_MODE" != external ]; then
        echo "SERVICE_MODE must be docker or external" >&2
        exit 2
    fi

    if [ "$SERVICE_MODE" = docker ]; then
        if ! command_exists docker || ! docker compose version >/dev/null 2>&1; then
            echo "Docker Compose is required in SERVICE_MODE=docker; use SERVICE_MODE=external with explicit *_HOST/*_PORT variables" >&2
            exit 2
        fi
        MYSQL_PORT=$(choose_free_port "$MYSQL_PORT")
        REDIS_PORT=$(choose_free_port "$REDIS_PORT")
        export MYSQL_PORT REDIS_PORT MYSQL_ROOT_PASSWORD
        # tr 'A-Z:' 'a-z--' (not just 'A-Z') because RUN_ID may contain ':'
        # (an ISO-8601 timestamp) or other characters a Compose project name
        # cannot carry; same as tests/frameworks/symfony/run.sh.
        COMPOSE_PROJECT="fpmng-slim4-$(printf '%s' "$RUN_ID" | tr 'A-Z:' 'a-z--')"
        COMPOSE=(docker compose -f "$ROOT/compose.yaml" -p "$COMPOSE_PROJECT")
        if ! "${COMPOSE[@]}" up -d >"$RUN_DIR/compose.log" 2>&1; then
            echo "Docker Compose could not start MySQL and Redis; see $RUN_DIR/compose.log" >&2
            exit 2
        fi
        DOCKER_STARTED=1
        local attempt
        local ready=0
        for attempt in $(seq 1 90); do
            if "${COMPOSE[@]}" exec -T mysql mysqladmin ping --protocol=tcp -h 127.0.0.1 \
                -uroot -p"$MYSQL_ROOT_PASSWORD" >/dev/null 2>&1 \
                && "${COMPOSE[@]}" exec -T redis redis-cli ping >/dev/null 2>&1; then
                ready=1
                break
            fi
            sleep 1
        done
        if [ "$ready" -ne 1 ]; then
            echo "Docker services did not become ready; see $RUN_DIR/compose.log" >&2
            exit 2
        fi
        SLIM_DB_HOST=127.0.0.1
        SLIM_DB_PORT=$MYSQL_PORT
        SLIM_DB_USER=root
        SLIM_DB_PASSWORD=$MYSQL_ROOT_PASSWORD
        # A per-run database name, same discipline as the Symfony runner:
        # never collide with, or reuse state from, another run. Sanitized
        # the same way (setup.php requires ^[A-Za-z0-9_]+$ for DB names, and
        # RUN_ID is caller-controllable via FPMNG_RUN_ID).
        SLIM_DB_NAME="slim4_${RUN_ID//[^A-Za-z0-9]/_}"
        SLIM_REDIS_HOST=127.0.0.1
        SLIM_REDIS_PORT=$REDIS_PORT
        SLIM_REDIS_DB=0
    else
        SLIM_DB_HOST=${SLIM_DB_HOST:-127.0.0.1}
        SLIM_DB_PORT=${SLIM_DB_PORT:-3306}
        SLIM_DB_NAME=${SLIM_DB_NAME:-slim4}
        SLIM_DB_USER=${SLIM_DB_USER:-bench}
        SLIM_DB_PASSWORD=${SLIM_DB_PASSWORD:-bench}
        SLIM_REDIS_HOST=${SLIM_REDIS_HOST:-127.0.0.1}
        SLIM_REDIS_PORT=${SLIM_REDIS_PORT:-6379}
        SLIM_REDIS_DB=${SLIM_REDIS_DB:-2}
    fi
}

mkdir -p "$RUN_DIR/sessions"
# Installed before setup_services (which can set DOCKER_STARTED=1 and then
# exit 2 on a later precondition) so a Docker Compose project is never left
# running past this script's exit. Replaced with the fuller cleanup() trap
# further down.
trap cleanup_services EXIT INT TERM
setup_services
FCGI_PORT=$(choose_free_port "$FCGI_PORT")
# Search for HTTP_PORT starting strictly after the FCGI port that was
# actually chosen: two independent choose_free_port searches only 1 apart
# by default (22625/22626) can both land on the same bumped port when the
# default FCGI port is occupied, producing a broken pool config (listen and
# http.listen on the same port).
if (( HTTP_PORT <= FCGI_PORT )); then
    HTTP_PORT=$((FCGI_PORT + 1))
fi
HTTP_PORT=$(choose_free_port "$HTTP_PORT")
if [ "$SLIM_ROUTE_CACHE" = 1 ]; then
    rm -f "$SLIM_ROUTE_CACHE_FILE"
fi

cat > "$RUN_DIR/php.ini" <<INI
display_errors=1
error_reporting=E_ALL
log_errors=1
error_log=$RUN_DIR/php-errors.log
memory_limit=256M
date.timezone=UTC
session.save_handler=files
session.save_path=$RUN_DIR/sessions
session.gc_probability=0
INI

cat > "$RUN_DIR/fpm.conf" <<CONF
[global]
pid = $RUN_DIR/php-fpm-ng.pid
error_log = $RUN_DIR/php-fpm-ng.log
log_level = notice
daemonize = yes

[www]
listen = 127.0.0.1:$FCGI_PORT
pool.type = http
http.listen = 127.0.0.1:$HTTP_PORT
http.access_log = $RUN_DIR/http.access
pool.executor = fiber
pm = static
pm.max_children = 1
chdir = $ROOT/public
php_admin_flag[opcache.enable] = off
php_admin_value[max_execution_time] = 0
catch_workers_output = yes
env[FPMNG_SHARED_INCLUDES] = 1
env[SLIM_CONTAINER] = "$SLIM_CONTAINER"
env[SLIM_ROUTE_CACHE] = "$SLIM_ROUTE_CACHE"
env[SLIM_ROUTE_CACHE_FILE] = "$SLIM_ROUTE_CACHE_FILE"
env[SLIM_DB_HOST] = $SLIM_DB_HOST
env[SLIM_DB_PORT] = $SLIM_DB_PORT
env[SLIM_DB_NAME] = $SLIM_DB_NAME
env[SLIM_DB_USER] = $SLIM_DB_USER
env[SLIM_DB_PASSWORD] = $SLIM_DB_PASSWORD
env[SLIM_REDIS_HOST] = $SLIM_REDIS_HOST
env[SLIM_REDIS_PORT] = $SLIM_REDIS_PORT
env[SLIM_REDIS_DB] = $SLIM_REDIS_DB
CONF

cleanup() {
    if [ -f "$RUN_DIR/php-fpm-ng.pid" ]; then
        pid=$(cat "$RUN_DIR/php-fpm-ng.pid")
        if kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
            for _ in $(seq 1 50); do
                kill -0 "$pid" 2>/dev/null || break
                sleep 0.1
            done
            kill -KILL "$pid" 2>/dev/null || true
        fi
    fi
    cleanup_services
}
trap cleanup EXIT INT TERM

SLIM_DB_HOST="$SLIM_DB_HOST" \
SLIM_DB_PORT="$SLIM_DB_PORT" \
SLIM_DB_NAME="$SLIM_DB_NAME" \
SLIM_DB_USER="$SLIM_DB_USER" \
SLIM_DB_PASSWORD="$SLIM_DB_PASSWORD" \
"$PHP" "$ROOT/bin/setup.php"

"$FPMNG" -c "$RUN_DIR/php.ini" -y "$RUN_DIR/fpm.conf"

for _ in $(seq 1 100); do
    if curl --silent --show-error --fail "http://127.0.0.1:$HTTP_PORT/health" > "$RUN_DIR/health.json"; then
        break
    fi
    sleep 0.1
done

if ! grep -q '"ok":true' "$RUN_DIR/health.json"; then
    echo "php-fpm-ng did not become ready" >&2
    cat "$RUN_DIR/php-fpm-ng.log" >&2 || true
    exit 1
fi

SLIM_BASE_URL="http://127.0.0.1:$HTTP_PORT" \
SLIM_CONTAINER="$SLIM_CONTAINER" \
SLIM_ROUTE_CACHE="$SLIM_ROUTE_CACHE" \
SLIM_ROUTE_CACHE_FILE="$SLIM_ROUTE_CACHE_FILE" \
"$PHP" "$ROOT/bin/run.php"
