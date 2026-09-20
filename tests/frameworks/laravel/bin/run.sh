#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
RUN_DIR=${RUN_DIR:-$ROOT/.run}
PHP=${PHP:-php}
FPMNG=${FPMNG:-php-fpm-ng}
FCGI_PORT=${FCGI_PORT:-22725}
HTTP_PORT=${HTTP_PORT:-22726}
# SERVICE_MODE=docker (default) provisions a private MySQL/Redis via
# compose.yaml so the runner works on a clean machine with nothing
# pre-provisioned but Docker. SERVICE_MODE=external keeps the previous
# behaviour of pointing at already-running services via the LARAVEL_DB_*
# and LARAVEL_REDIS_* variables below (e.g. the shared test box).
SERVICE_MODE=${SERVICE_MODE:-docker}
# Issue #51: the negative controls are DESIGNED to corrupt their database (with
# an empty static list, concurrent requests share state and the probe writes
# garbage at whatever MySQL it can reach). They must therefore never point at a
# MySQL other work shares. SERVICE_MODE=docker already provisions a private one;
# in SERVICE_MODE=external the operator must assert the services are private
# before the negative phase will run, or it is skipped.
LARAVEL_NEGATIVE_ALLOW_EXTERNAL=${LARAVEL_NEGATIVE_ALLOW_EXTERNAL:-0}
MYSQL_PORT=${MYSQL_PORT:-13307}
REDIS_PORT=${REDIS_PORT:-16380}
MYSQL_ROOT_PASSWORD=${MYSQL_ROOT_PASSWORD:-fpmng-laravel-root}
LARAVEL_DB_HOST=${LARAVEL_DB_HOST:-127.0.0.1}
LARAVEL_DB_PORT=${LARAVEL_DB_PORT:-3306}
LARAVEL_DB_NAME=${LARAVEL_DB_NAME:-laravel025}
LARAVEL_DB_USER=${LARAVEL_DB_USER:-bench}
LARAVEL_DB_PASSWORD=${LARAVEL_DB_PASSWORD:-bench}
LARAVEL_REDIS_HOST=${LARAVEL_REDIS_HOST:-127.0.0.1}
LARAVEL_REDIS_PORT=${LARAVEL_REDIS_PORT:-6379}
LARAVEL_REDIS_DB=${LARAVEL_REDIS_DB:-3}
REDIS_CLIENT=${REDIS_CLIENT:-phpredis}
REDIS_EXTENSION=${REDIS_EXTENSION:-}
# Laravel 13.30.1, verified by tests/frameworks/laravel. Entries 1-3 were
# found by the session/auth scenarios (task 008), entry 4 by the Eloquent
# probe, entries 5-6 by the task 025 observers/global-scopes scenario: with
# only four entries, a per-request registered global scope from request A
# silently filters request B's query (HTTP 200, item:null), and model events
# dispatch through whichever request's event dispatcher was cached last.
STATIC_LIST=${STATIC_LIST:-Illuminate\\Container\\Container::instance,Illuminate\\Support\\Facades\\Facade::app,Illuminate\\Support\\Facades\\Facade::resolvedInstance,Illuminate\\Database\\Eloquent\\Model::resolver,Illuminate\\Database\\Eloquent\\Model::dispatcher,Illuminate\\Database\\Eloquent\\Model::globalScopes}
DOCKER_STARTED=0
RUN_ID=${FPMNG_RUN_ID:-$(date -u +%Y%m%dt%H%M%Sz)_$$}
COMPOSE=()

for command in curl nc; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "$command is required (port checks and the HTTP concurrency runner need it)" >&2
        exit 2
    fi
done

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

# Split out from cleanup() (defined later, once stop_pool exists) so it can
# be trapped immediately around setup_services: any exit between "docker
# compose up" and the full cleanup() trap being installed would otherwise
# leak the compose project and its volume forever.
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
        COMPOSE_PROJECT="fpmng-laravel-$(printf '%s' "$RUN_ID" | tr 'A-Z:' 'a-z--')"
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
        LARAVEL_DB_HOST=127.0.0.1
        LARAVEL_DB_PORT=$MYSQL_PORT
        LARAVEL_DB_USER=root
        LARAVEL_DB_PASSWORD=$MYSQL_ROOT_PASSWORD
        # A per-run database name, same discipline as the Symfony runner:
        # never collide with, or reuse state from, another run. Sanitized
        # the same way (setup.php requires ^[A-Za-z0-9_]+$ for DB names, and
        # RUN_ID is caller-controllable via FPMNG_RUN_ID).
        LARAVEL_DB_NAME="laravel025_${RUN_ID//[^A-Za-z0-9]/_}"
        LARAVEL_REDIS_HOST=127.0.0.1
        LARAVEL_REDIS_PORT=$REDIS_PORT
        LARAVEL_REDIS_DB=0
    fi
}

if [ ! -f "$ROOT/vendor/autoload.php" ]; then
    PHP="$PHP" RUN_DIR="$RUN_DIR" "$ROOT/bin/provision.sh"
fi
if [ ! -f "$ROOT/vendor/autoload.php" ]; then
    echo "Laravel dependency provisioning did not create vendor/autoload.php" >&2
    exit 2
fi

mkdir -p "$RUN_DIR"
# Installed before setup_services (which can set DOCKER_STARTED=1 and then
# exit 2 on a later precondition, e.g. the phpredis check below) so a
# Docker Compose project is never left running past this script's exit.
# Replaced with the fuller cleanup() trap further down, once stop_pool
# exists.
trap cleanup_services EXIT INT TERM
setup_services
FCGI_PORT=$(choose_free_port "$FCGI_PORT")
# Search for HTTP_PORT starting strictly after the FCGI port that was
# actually chosen: two independent choose_free_port searches only 1 apart
# by default (22725/22726) can both land on the same bumped port when the
# default FCGI port is occupied, producing a broken pool config (listen and
# http.listen on the same port).
if (( HTTP_PORT <= FCGI_PORT )); then
    HTTP_PORT=$((FCGI_PORT + 1))
fi
HTTP_PORT=$(choose_free_port "$HTTP_PORT")

if [ "$REDIS_CLIENT" = phpredis ]; then
    if [ -z "$REDIS_EXTENSION" ] || [ ! -f "$REDIS_EXTENSION" ]; then
        echo "REDIS_CLIENT=phpredis requires REDIS_EXTENSION pointing to a compatible redis.so" >&2
        exit 2
    fi
    redis_version=$($PHP -d "extension=$REDIS_EXTENSION" -r 'echo phpversion("redis"), "\n";' 2>&1) || {
        echo "could not load REDIS_EXTENSION=$REDIS_EXTENSION: $redis_version" >&2
        exit 2
    }
    if [ -z "$redis_version" ]; then
        echo "REDIS_EXTENSION did not expose the redis extension" >&2
        exit 2
    fi
else
    redis_version="not loaded (Laravel client: $REDIS_CLIENT)"
fi

mkdir -p "$RUN_DIR" "$RUN_DIR/sessions" "$RUN_DIR/storage/framework/cache/data" \
    "$RUN_DIR/storage/framework/sessions" "$RUN_DIR/storage/framework/views" \
    "$RUN_DIR/storage/logs" "$RUN_DIR/storage/app/private" "$ROOT/bootstrap/cache" \
    "$ROOT/storage/framework/cache/data" "$ROOT/storage/framework/sessions" \
    "$ROOT/storage/framework/views" "$ROOT/storage/logs" "$ROOT/storage/app/private"
: > "$RUN_DIR/php-errors.log"
: > "$RUN_DIR/php-fpm-ng-configured.log"
: > "$RUN_DIR/php-fpm-ng-negative.log"
rm -f "$RUN_DIR"/results-*.log "$RUN_DIR"/health-*.json

cat > "$ROOT/.env" <<ENV
APP_NAME=Laravel025Probe
APP_ENV=testing
APP_KEY=base64:3o1X1nqQvYj8D4y4p9m5f2a1b0c7d6e5f4g3h2i1j0k=
APP_DEBUG=false
APP_URL=http://127.0.0.1:$HTTP_PORT
LOG_CHANNEL=single
LOG_LEVEL=debug
DB_CONNECTION=mysql
DB_HOST=$LARAVEL_DB_HOST
DB_PORT=$LARAVEL_DB_PORT
DB_DATABASE=$LARAVEL_DB_NAME
DB_USERNAME=$LARAVEL_DB_USER
DB_PASSWORD=$LARAVEL_DB_PASSWORD
SESSION_DRIVER=redis
SESSION_LIFETIME=120
SESSION_COOKIE=laravel025_session
CACHE_STORE=redis
CACHE_PREFIX=laravel025-cache-
QUEUE_CONNECTION=sync
BROADCAST_CONNECTION=log
REDIS_CLIENT=$REDIS_CLIENT
REDIS_HOST=$LARAVEL_REDIS_HOST
REDIS_PORT=$LARAVEL_REDIS_PORT
REDIS_DB=$LARAVEL_REDIS_DB
REDIS_CACHE_DB=$LARAVEL_REDIS_DB
REDIS_QUEUE_DB=$LARAVEL_REDIS_DB
REDIS_PREFIX=laravel025:
REDIS_PASSWORD=
ENV

php_ini="$RUN_DIR/php.ini"
cat > "$php_ini" <<INI
memory_limit=256M
date.timezone=UTC
display_errors=1
error_reporting=E_ALL
log_errors=1
error_log=$RUN_DIR/php-errors.log
variables_order=GPCS
INI
if [ "$REDIS_CLIENT" = phpredis ]; then
    printf 'extension=%s\n' "$REDIS_EXTENSION" >> "$php_ini"
fi

cat > "$RUN_DIR/fpm.conf.template" <<'CONF'
[global]
pid = __PID__
error_log = __FPM_LOG__
log_level = notice
daemonize = yes

[gw]
pool.type = gateway
listen = 127.0.0.1:__HTTP_PORT__
http.access_log = __HTTP_LOG__
http.route[www] = /

[www]
pool.type = fastcgi
listen = 127.0.0.1:__FCGI_PORT__
pool.executor = fiber
pm = static
pm.max_children = 1
chdir = __ROOT__/public
php_admin_flag[opcache.enable] = off
php_admin_value[max_execution_time] = 0
catch_workers_output = yes
env[FPMNG_SHARED_INCLUDES] = 1
env[REDIS_CLIENT] = __REDIS_CLIENT__
env[REDIS_HOST] = __REDIS_HOST__
env[REDIS_PORT] = __REDIS_PORT__
env[REDIS_DB] = __REDIS_DB__
env[REDIS_CACHE_DB] = __REDIS_DB__
env[REDIS_QUEUE_DB] = __REDIS_DB__
env[DB_HOST] = __DB_HOST__
env[DB_PORT] = __DB_PORT__
env[DB_DATABASE] = __DB_NAME__
env[DB_USERNAME] = __DB_USER__
env[DB_PASSWORD] = __DB_PASSWORD__
env[APP_URL] = http://127.0.0.1:__HTTP_PORT__
env[APP_ENV] = testing
env[LOG_CHANNEL] = single
__STATIC_LINE__
CONF

write_fpm_config() {
    local mode=$1
    local list=$2

    cat > "$RUN_DIR/fpm-$mode.conf" <<CONF
[global]
pid = $RUN_DIR/php-fpm-ng-$mode.pid
error_log = $RUN_DIR/php-fpm-ng-$mode.log
log_level = notice
daemonize = yes

[gw]
pool.type = gateway
listen = 127.0.0.1:$HTTP_PORT
http.access_log = $RUN_DIR/http-$mode.access
http.route[www] = /

[www]
pool.type = fastcgi
listen = 127.0.0.1:$FCGI_PORT
pool.executor = fiber
pm = static
pm.max_children = 1
chdir = $ROOT/public
php_admin_flag[opcache.enable] = off
php_admin_value[max_execution_time] = 0
catch_workers_output = yes
env[FPMNG_SHARED_INCLUDES] = 1
env[REDIS_CLIENT] = $REDIS_CLIENT
env[REDIS_HOST] = $LARAVEL_REDIS_HOST
env[REDIS_PORT] = $LARAVEL_REDIS_PORT
env[REDIS_DB] = $LARAVEL_REDIS_DB
env[REDIS_CACHE_DB] = $LARAVEL_REDIS_DB
env[REDIS_QUEUE_DB] = $LARAVEL_REDIS_DB
env[DB_HOST] = $LARAVEL_DB_HOST
env[DB_PORT] = $LARAVEL_DB_PORT
env[DB_DATABASE] = $LARAVEL_DB_NAME
env[DB_USERNAME] = $LARAVEL_DB_USER
env[DB_PASSWORD] = $LARAVEL_DB_PASSWORD
env[APP_URL] = http://127.0.0.1:$HTTP_PORT
env[APP_ENV] = testing
env[LOG_CHANNEL] = single
CONF
    if [ -n "$list" ]; then
        printf 'fiber.isolate_statics = %s\n' "$list" >> "$RUN_DIR/fpm-$mode.conf"
    fi
}

stop_pool() {
    local mode=$1
    local pid_file="$RUN_DIR/php-fpm-ng-$mode.pid"
    if [ ! -f "$pid_file" ]; then
        return 0
    fi

    local pid
    pid=$(cat "$pid_file")
    if kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        for _ in $(seq 1 100); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$pid" 2>/dev/null; then
            kill -KILL "$pid" 2>/dev/null || true
        fi
    fi
    rm -f "$pid_file"
}

start_pool() {
    local mode=$1
    local list=$2
    write_fpm_config "$mode" "$list"
    "$FPMNG" -c "$php_ini" -y "$RUN_DIR/fpm-$mode.conf"

    for _ in $(seq 1 120); do
        if curl --silent --show-error --fail --max-time 2 "http://127.0.0.1:$HTTP_PORT/up" > "$RUN_DIR/health-$mode.json"; then
            return 0
        fi
        sleep 0.1
    done

    echo "php-fpm-ng did not become ready for $mode" >&2
    cat "$RUN_DIR/php-fpm-ng-$mode.log" >&2 || true
    cat "$RUN_DIR/php-errors.log" >&2 || true
    return 1
}

run_suite() {
    local mode=$1
    local scenario=$2
    set +e
    LARAVEL_TEST_MODE="$mode" \
    LARAVEL_ONLY="$scenario" \
    LARAVEL_BASE_URL="http://127.0.0.1:$HTTP_PORT" \
    "$PHP" "$ROOT/bin/run.php" 2>&1 | tee "$RUN_DIR/results-$mode-$scenario.log"
    local status=${PIPESTATUS[0]}
    set -e
    return "$status"
}

CONFIGURED_PASS=0
CONFIGURED_ERROR=0
NEGATIVE_PASS=0
NEGATIVE_ERROR=0

run_scenarios() {
    local mode=$1
    local list=$2
    shift 2
    local overall=0
    local scenario

    for scenario in "$@"; do
        if ! start_pool "$mode" "$list"; then
            overall=2
            if [ "$mode" = configured ]; then
                CONFIGURED_ERROR=$((CONFIGURED_ERROR + 1))
            elif [ "$mode" = negative ]; then
                NEGATIVE_ERROR=$((NEGATIVE_ERROR + 1))
            fi
            continue
        fi
        if run_suite "$mode" "$scenario"; then
            if [ "$mode" = configured ]; then
                CONFIGURED_PASS=$((CONFIGURED_PASS + 1))
            elif [ "$mode" = negative ]; then
                NEGATIVE_PASS=$((NEGATIVE_PASS + 1))
            fi
        else
            overall=1
            if [ "$mode" = configured ]; then
                CONFIGURED_ERROR=$((CONFIGURED_ERROR + 1))
            elif [ "$mode" = negative ]; then
                NEGATIVE_ERROR=$((NEGATIVE_ERROR + 1))
            fi
        fi
        stop_pool "$mode" || true
    done

    return "$overall"
}

cleanup() {
    stop_pool configured || true
    stop_pool negative || true
    # The audit pool is started inside run_scenarios like the others; without
    # this line an interrupt between its start and stop would leave a
    # daemonized php-fpm-ng alive on the (shared) test box.
    stop_pool audit || true
    rm -f "$ROOT/.env"
    cleanup_services
}
trap cleanup EXIT INT TERM

printf 'FPMNG=%s\n' "$FPMNG"
"$FPMNG" -v
printf 'FPMNG_SHA256='; sha256sum "$FPMNG" | awk '{print $1}'
printf 'FPMNG_STRINGS:\n'
strings "$FPMNG" | grep -E 'fiber\.isolate_statics|FPMNG_SHARED_INCLUDES|pool\.executor' | head -n 20
printf 'PHP=%s\n' "$PHP"
"$PHP" -v | head -n 3
printf 'PHP_MODULES='; "$PHP" -m | tr '\n' ' '; printf '\n'
printf 'REDIS_EXTENSION_VERSION=%s\n' "$redis_version"
if [ -n "${FPMNG_SOURCE_DIR:-}" ] && [ -d "$FPMNG_SOURCE_DIR/.git" ]; then
    printf 'FPMNG_SOURCE_HEAD='; git -C "$FPMNG_SOURCE_DIR" rev-parse HEAD
    printf 'FPMNG_SOURCE_DIR=%s\n' "$FPMNG_SOURCE_DIR"
    if [ -n "$(git -C "$FPMNG_SOURCE_DIR" status --porcelain)" ]; then
        printf 'FPMNG_SOURCE_DIRTY=yes\n'
    else
        printf 'FPMNG_SOURCE_DIRTY=no\n'
    fi
fi
printf 'LARAVEL_VERSION='; "$PHP" -r "require '$ROOT/vendor/autoload.php'; echo (new ReflectionClass('Illuminate\\\\Foundation\\\\Application'))->getConstant('VERSION'), PHP_EOL;"
printf 'PORTS fastcgi=%s http=%s\n' "$FCGI_PORT" "$HTTP_PORT"
printf 'RESOURCES mysql=%s redis_db=%s\n' "$LARAVEL_DB_NAME" "$LARAVEL_REDIS_DB"

LARAVEL_DB_HOST="$LARAVEL_DB_HOST" \
LARAVEL_DB_PORT="$LARAVEL_DB_PORT" \
LARAVEL_DB_NAME="$LARAVEL_DB_NAME" \
LARAVEL_DB_USER="$LARAVEL_DB_USER" \
LARAVEL_DB_PASSWORD="$LARAVEL_DB_PASSWORD" \
"$PHP" "$ROOT/bin/setup.php"

configured_status=0
negative_status=0
negative_skipped=0

configured_scenarios=(
    session-rounds
    authenticated-route
    mix-mysql-cache-redis-eloquent
    facade-and-object-identity
    eloquent-connection-resolver
    middleware-and-terminate
    csrf-session-isolation
    validation-flash-session-isolation
    queue-sync-context
    broadcast-sync-context
    rate-limiter
    mail-attribution
    view-blade-composer
    route-model-binding
    eloquent-observers-and-global-scopes
)
negative_scenarios=(
    negative-session-empty-static-list
    negative-authenticated-empty-static-list
    negative-mix-empty-static-list
    negative-facade-and-object-identity-empty-static-list
    negative-eloquent-connection-resolver-empty-static-list
    negative-middleware-and-terminate-empty-static-list
    negative-csrf-session-isolation-empty-static-list
    negative-validation-flash-session-isolation-empty-static-list
    negative-queue-sync-context-empty-static-list
    negative-broadcast-sync-context-empty-static-list
    negative-rate-limiter-empty-static-list
    negative-mail-attribution-empty-static-list
    negative-view-blade-composer-empty-static-list
    negative-route-model-binding-empty-static-list
    negative-eloquent-observers-and-global-scopes-empty-static-list
)

if ! run_scenarios configured "$STATIC_LIST" "${configured_scenarios[@]}"; then
    configured_status=1
fi
if [ "$SERVICE_MODE" = docker ] || [ "$LARAVEL_NEGATIVE_ALLOW_EXTERNAL" = 1 ]; then
    if ! run_scenarios negative "" "${negative_scenarios[@]}"; then
        negative_status=1
    fi
else
    # Issue #51: external services are assumed shared until the operator says
    # otherwise. The configured phase is read-mostly and fine either way; the
    # negative phase is not, so it is skipped rather than aimed at the shared
    # test-box MySQL (which logged RSET_HEADER protocol corruption when this
    # ran against it on 2026-09-08).
    negative_skipped=1
    echo "SKIP: negative scenarios not run under SERVICE_MODE=$SERVICE_MODE." >&2
    echo "      The negative controls are designed to corrupt their database, so they must not" >&2
    echo "      point at shared services. Use SERVICE_MODE=docker (the default), or set" >&2
    echo "      LARAVEL_NEGATIVE_ALLOW_EXTERNAL=1 if the external MySQL/Redis are private (issue #51)." >&2
fi

# Systematic statics audit (task 025). ONE run of the /statics-audit probe
# against a pool with the configured list: the probe touches every
# state-keeping subsystem, so any static that still changes across a real
# suspension is state the list does not cover — that is how the list is
# verified systematically instead of by hand-picking.
#
# An empty-list audit run was tried and removed: with no isolation the probe
# itself destabilizes the pool (17 of 64 audit requests came back 502, and
# the MySQL client logged RSET_HEADER protocol corruption — traffic aimed at
# the SHARED MySQL server on the test box, which the box rules ask us not to
# abuse). The per-scenario negative controls already reproduce empty-list
# damage on isolated routes; enumeration is what the clean audit is for.
# LARAVEL_AUDIT_EXPECT=leak remains available in run.php for one-off
# forensics on a private MySQL.
audit_status=0
export LARAVEL_ISOLATED_LIST="$STATIC_LIST"
export LARAVEL_AUDIT_EXPECT=clean
if ! run_scenarios audit "$STATIC_LIST" statics-audit; then
    audit_status=1
fi
unset LARAVEL_AUDIT_EXPECT LARAVEL_ISOLATED_LIST

printf 'RUN_STATUS configured=%s negative=%s audit=%s\n' "$configured_status" "$negative_status" "$audit_status"
printf 'SUMMARY configured_pass=%s configured_error=%s negative_pass=%s negative_error=%s negative_skipped=%s not_measured=0 audit_status=%s\n' \
    "$CONFIGURED_PASS" "$CONFIGURED_ERROR" "$NEGATIVE_PASS" "$NEGATIVE_ERROR" "$negative_skipped" "$audit_status"
if [ "$configured_status" -ne 0 ] || [ "$negative_status" -ne 0 ] || [ "$audit_status" -ne 0 ]; then
    exit 1
fi
