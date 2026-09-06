#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
RUN_DIR=${RUN_DIR:-$ROOT/.run}
PHP=${PHP:-php}
FPMNG=${FPMNG:-php-fpm-ng}
HTTP_PORT=${HTTP_PORT:-22626}
FCGI_PORT=${FCGI_PORT:-22625}

if [ ! -f "$ROOT/vendor/autoload.php" ]; then
    echo "Slim 4 dependencies are missing; run composer install in $ROOT" >&2
    exit 2
fi

mkdir -p "$RUN_DIR/sessions"

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
env[SLIM_DB_HOST] = ${SLIM_DB_HOST:-127.0.0.1}
env[SLIM_DB_PORT] = ${SLIM_DB_PORT:-3306}
env[SLIM_DB_NAME] = ${SLIM_DB_NAME:-slim4}
env[SLIM_DB_USER] = ${SLIM_DB_USER:-bench}
env[SLIM_DB_PASSWORD] = ${SLIM_DB_PASSWORD:-bench}
env[SLIM_REDIS_HOST] = ${SLIM_REDIS_HOST:-127.0.0.1}
env[SLIM_REDIS_PORT] = ${SLIM_REDIS_PORT:-6379}
env[SLIM_REDIS_DB] = ${SLIM_REDIS_DB:-2}
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
}
trap cleanup EXIT INT TERM

SLIM_DB_HOST=${SLIM_DB_HOST:-127.0.0.1} \
SLIM_DB_PORT=${SLIM_DB_PORT:-3306} \
SLIM_DB_NAME=${SLIM_DB_NAME:-slim4} \
SLIM_DB_USER=${SLIM_DB_USER:-bench} \
SLIM_DB_PASSWORD=${SLIM_DB_PASSWORD:-bench} \
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
"$PHP" "$ROOT/bin/run.php"
