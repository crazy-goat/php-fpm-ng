#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
RUN_DIR=${RUN_DIR:-$ROOT/.run}
PHP=${PHP:-php}
FPMNG=${FPMNG:-php-fpm-ng}
FCGI_PORT=${FCGI_PORT:-22725}
HTTP_PORT=${HTTP_PORT:-22726}
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
STATIC_LIST=${STATIC_LIST:-Illuminate\\Container\\Container::instance,Illuminate\\Support\\Facades\\Facade::app,Illuminate\\Support\\Facades\\Facade::resolvedInstance,Illuminate\\Database\\Eloquent\\Model::resolver}

if ! command -v curl >/dev/null 2>&1; then
    echo "curl is required by the HTTP concurrency runner and Composer provisioner" >&2
    exit 2
fi

if [ ! -f "$ROOT/vendor/autoload.php" ]; then
    PHP="$PHP" RUN_DIR="$RUN_DIR" "$ROOT/bin/provision.sh"
fi
if [ ! -f "$ROOT/vendor/autoload.php" ]; then
    echo "Laravel dependency provisioning did not create vendor/autoload.php" >&2
    exit 2
fi

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

if command -v ss >/dev/null 2>&1; then
    if ss -lnt | grep -Eq ":(${FCGI_PORT}|${HTTP_PORT})[[:space:]]"; then
        echo "one of the Laravel probe ports is already occupied: FastCGI=$FCGI_PORT HTTP=$HTTP_PORT" >&2
        exit 2
    fi
fi

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

[www]
listen = 127.0.0.1:__FCGI_PORT__
pool.type = http
http.listen = 127.0.0.1:__HTTP_PORT__
http.access_log = __HTTP_LOG__
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

[www]
listen = 127.0.0.1:$FCGI_PORT
pool.type = http
http.listen = 127.0.0.1:$HTTP_PORT
http.access_log = $RUN_DIR/http-$mode.access
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
            else
                NEGATIVE_ERROR=$((NEGATIVE_ERROR + 1))
            fi
            continue
        fi
        if run_suite "$mode" "$scenario"; then
            if [ "$mode" = configured ]; then
                CONFIGURED_PASS=$((CONFIGURED_PASS + 1))
            else
                NEGATIVE_PASS=$((NEGATIVE_PASS + 1))
            fi
        else
            overall=1
            if [ "$mode" = configured ]; then
                CONFIGURED_ERROR=$((CONFIGURED_ERROR + 1))
            else
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
    rm -f "$ROOT/.env"
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
)

if ! run_scenarios configured "$STATIC_LIST" "${configured_scenarios[@]}"; then
    configured_status=1
fi
if ! run_scenarios negative "" "${negative_scenarios[@]}"; then
    negative_status=1
fi

printf 'RUN_STATUS configured=%s negative=%s\n' "$configured_status" "$negative_status"
printf 'SUMMARY configured_pass=%s configured_error=%s negative_pass=%s negative_error=%s not_measured=4\n' \
    "$CONFIGURED_PASS" "$CONFIGURED_ERROR" "$NEGATIVE_PASS" "$NEGATIVE_ERROR"
if [ "$configured_status" -ne 0 ] || [ "$negative_status" -ne 0 ]; then
    exit 1
fi
