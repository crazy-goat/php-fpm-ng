#!/usr/bin/env bash
set -Eeuo pipefail

# Run the repository-owned Symfony probe against one explicitly supplied
# php-fpm-ng binary. The process and every service used by this script have a
# run-specific directory, port, database, Redis prefix, and cleanup path.

SUITE_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=$(cd -- "$SUITE_DIR/../../.." && pwd)
RUN_ID=${FPMNG_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)-$$}
RUN_ROOT=${FPMNG_RUN_ROOT:-$SUITE_DIR/.runs/$RUN_ID}
APP_DIR=$RUN_ROOT/app
RESULT_ROOT=$RUN_ROOT/results
mkdir -p "$APP_DIR" "$RESULT_ROOT"

FPMNG_BIN=${FPMNG_BIN:-}
SERVICE_MODE=${SERVICE_MODE:-docker}
COUNT=${FPMNG_SCENARIO_COUNT:-8}
HTTP_TIMEOUT=${FPMNG_HTTP_TIMEOUT:-120}
MYSQL_PORT=${MYSQL_PORT:-13306}
REDIS_PORT=${REDIS_PORT:-16379}
REDIS_DB=${REDIS_DB:-15}
FPM_HTTP_PORT=${FPM_HTTP_PORT:-18080}
FPM_FCGI_PORT=${FPM_FCGI_PORT:-19000}
MYSQL_ROOT_PASSWORD=${MYSQL_ROOT_PASSWORD:-fpmng-symfony-root}

FPM_PID=
DB_CREATED=0
DOCKER_STARTED=0
CLEANUP_DONE=0
SOURCE_COMMIT=unknown
BINARY_SHA=unknown
BINARY_VERSION=unknown
BINARY_MARKERS=FPMNG_SHARED_INCLUDES,http.front_controller,fiber.revalidate_freq,fiber.isolate_statics
REDIS_CLIENT=predis
BASE_URL=
ACTIVE_PREFIX=
ACTIVE_RESULT_ROOT=$RESULT_ROOT
CURRENT_ENV=dev
START_POOL_SEQ=0

SCENARIO_NAMES=(
    mix
    session
    stateful-auth
    object-identity
    prod-mix
    prod-session
    prod-stateful-auth
    prod-object-identity
    negative-shared-includes
    unsupported-configuration
    pm2-mix
    pm2-session
    pm2-stateful-auth
    pm2-object-identity
    framework-twig
    framework-form-validation
    framework-messenger
    rss-stability
    fiber-revalidate
    app-env-comparison
    pm-max-children
)
SCENARIO_STATUS=()
SCENARIO_DETAIL=()

result_name() {
    printf '%s%s' "$ACTIVE_PREFIX" "$1"
}

record() {
    local name=$1
    local status=$2
    local detail=${3:-}
    local index

    detail=${detail//$'\n'/ }
    detail=${detail//|/\/}
    for index in "${!SCENARIO_NAMES[@]}"; do
        if [[ ${SCENARIO_NAMES[$index]} == "$name" ]]; then
            SCENARIO_STATUS[$index]=$status
            SCENARIO_DETAIL[$index]=$detail
            return
        fi
    done
    SCENARIO_NAMES+=("$name")
    SCENARIO_STATUS+=("$status")
    SCENARIO_DETAIL+=("$detail")
}

mark_not_measured() {
    local reason=$1
    local name
    for name in "${SCENARIO_NAMES[@]}"; do
        [[ -n ${SCENARIO_STATUS[$(scenario_index "$name")]:-} ]] || record "$name" "NOT MEASURED" "$reason"
    done
}

scenario_index() {
    local wanted=$1
    local index
    for index in "${!SCENARIO_NAMES[@]}"; do
        [[ ${SCENARIO_NAMES[$index]} == "$wanted" ]] && { echo "$index"; return; }
    done
    echo -1
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

sha256_of() {
    if command_exists shasum; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        sha256sum "$1" | awk '{print $1}'
    fi
}

port_is_free() {
    ! nc -z 127.0.0.1 "$1" >/dev/null 2>&1
}

choose_free_port() {
    local port=$1
    while ! port_is_free "$port"; do
        port=$((port + 1))
    done
    echo "$port"
}

url_encode() {
    python3 - "$1" <<'PY'
import sys
from urllib.parse import quote
print(quote(sys.argv[1], safe=''))
PY
}

mysql_admin() {
    local sql=$1
    if [[ $DOCKER_STARTED -eq 1 ]]; then
        "${COMPOSE[@]}" exec -T mysql env MYSQL_PWD="$MYSQL_ADMIN_PASSWORD" mysql \
            --protocol=tcp -h 127.0.0.1 -uroot -e "$sql"
    elif [[ ${MYSQL_ADMIN_SUDO:-0} -eq 1 ]]; then
        sudo -n mysql --protocol=socket -e "$sql"
    else
        MYSQL_PWD="$MYSQL_ADMIN_PASSWORD" mysql --protocol=tcp -h "$MYSQL_HOST" -P "$MYSQL_PORT" \
            -u "$MYSQL_ADMIN_USER" -e "$sql"
    fi
}

redis_cli() {
    if [[ $DOCKER_STARTED -eq 1 ]]; then
        "${COMPOSE[@]}" exec -T redis redis-cli -n "$REDIS_DB" "$@"
    else
        redis-cli -h "$REDIS_HOST" -p "$REDIS_PORT" -n "$REDIS_DB" "$@"
    fi
}

stop_pid() {
    local pid=${1:-}
    local attempt
    [[ -n $pid ]] || return 0
    if kill -0 "$pid" >/dev/null 2>&1; then
        kill -TERM "$pid" >/dev/null 2>&1 || true
        for attempt in {1..50}; do
            kill -0 "$pid" >/dev/null 2>&1 || break
            sleep 0.1
        done
        if kill -0 "$pid" >/dev/null 2>&1; then
            kill -KILL "$pid" >/dev/null 2>&1 || true
        fi
    fi
    wait "$pid" >/dev/null 2>&1 || true
}

cleanup() {
    local status=$?
    [[ $CLEANUP_DONE -eq 1 ]] && return
    CLEANUP_DONE=1
    set +e

    stop_pid "$FPM_PID"
    FPM_PID=

    if [[ $DB_CREATED -eq 1 ]]; then
        mysql_admin "DROP DATABASE IF EXISTS $DB_NAME;" >/dev/null 2>&1 || true
        DB_CREATED=0
    fi

    if [[ $DOCKER_STARTED -eq 1 ]]; then
        "${COMPOSE[@]}" down --volumes --remove-orphans >/dev/null 2>&1 || true
        DOCKER_STARTED=0
    elif [[ -n ${REDIS_PREFIX:-} ]] && command_exists redis-cli; then
        redis_cli --scan --pattern "${REDIS_PREFIX}*" 2>/dev/null \
            | while IFS= read -r key; do [[ -n $key ]] && redis_cli del "$key" >/dev/null 2>&1; done
    fi

    return "$status"
}
trap cleanup EXIT INT TERM

write_pool_config() {
    local path=$1
    local http_port=$2
    local fcgi_port=$3
    local shared=$4
    local executor=${5:-fiber}
    local error_log=${6:-$RUN_ROOT/fpm.log}
    local children=${7:-1}
    local revalidate_freq=${8:-0}

    cat > "$path" <<EOF
[global]
daemonize = no
error_log = $error_log
pid = $RUN_ROOT/fpm.pid
log_level = notice

[probe]
user = $(id -un)
group = $(id -gn)
chdir = $APP_DIR/public
listen = 127.0.0.1:$fcgi_port
pm = static
pm.max_children = $children
catch_workers_output = yes
clear_env = no
pool.type = http
pool.executor = $executor
http.listen = 127.0.0.1:$http_port
http.front_controller = /index.php
php_admin_value[max_execution_time] = 0
php_admin_value[opcache.enable] = 0
EOF
    if (( revalidate_freq > 0 )); then
        printf 'fiber.revalidate_freq = %s\n' "$revalidate_freq" >> "$path"
    fi
    if [[ $shared -eq 1 ]]; then
        printf '%s\n' 'env[FPMNG_SHARED_INCLUDES] = 1' >> "$path"
    fi
}

write_unsupported_config() {
    local path=$1
    local fcgi_port=$2
    cat > "$path" <<EOF
[global]
daemonize = no
error_log = $RUN_ROOT/unsupported.log

[probe]
user = $(id -un)
group = $(id -gn)
chdir = $APP_DIR/public
listen = 127.0.0.1:$fcgi_port
pm = static
pm.max_children = 1
pool.type = fastcgi-ng
pool.executor = classic
fiber.isolate_statics = App\\Controller\\ProbeController::hits
php_admin_value[max_execution_time] = 0
EOF
}

write_async_config() {
    local path=$1
    local fcgi_port=$2
    cat > "$path" <<EOF
[global]
daemonize = no
error_log = $RUN_ROOT/async.log

[probe]
user = $(id -un)
group = $(id -gn)
chdir = $APP_DIR/public
listen = 127.0.0.1:$fcgi_port
pm = static
pm.max_children = 1
pool.type = fastcgi-ng
pool.executor = async
php_admin_value[max_execution_time] = 0
EOF
}

start_fpm() {
    local config=$1
    local shared=$2
    local log=$3

    if [[ $shared -eq 1 ]]; then
        "$FPMNG_BIN" "${FPM_OPTIONS[@]}" -F -O -y "$config" >"$log" 2>&1 &
    else
        env -u FPMNG_SHARED_INCLUDES "$FPMNG_BIN" "${FPM_OPTIONS[@]}" -F -O -y "$config" >"$log" 2>&1 &
    fi
    FPM_PID=$!
}

wait_for_port() {
    local port=$1
    local attempt
    for attempt in {1..300}; do
        if nc -z 127.0.0.1 "$port" >/dev/null 2>&1; then
            return 0
        fi
        if [[ -n $FPM_PID ]] && ! kill -0 "$FPM_PID" >/dev/null 2>&1; then
            return 1
        fi
        sleep 0.1
    done
    return 1
}

wait_for_health() {
    local attempt
    for attempt in {1..100}; do
        if curl --silent --show-error --connect-timeout 1 --max-time 2 "$BASE_URL/health" \
            >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

start_request() {
    local directory=$1
    local name=$2
    local url=$3
    shift 3
    curl --silent --show-error --http1.1 --connect-timeout 5 --max-time "$HTTP_TIMEOUT" \
        "$@" "$url" -o "$directory/$name.body" -w '%{http_code}' \
        >"$directory/$name.code" 2>"$directory/$name.err" &
    REQUEST_PIDS+=("$!")
}

wait_requests() {
    local pid
    local failed=0
    for pid in "${REQUEST_PIDS[@]}"; do
        if ! wait "$pid"; then
            failed=1
        fi
    done
    REQUEST_PIDS=()
    return "$failed"
}

stop_requests() {
    local pid
    for pid in "${REQUEST_PIDS[@]}"; do
        kill -TERM "$pid" >/dev/null 2>&1 || true
    done
    wait_requests || true
}

prepare_gate() {
    local name=$1
    GATE_READY_KEY="${REDIS_PREFIX}gate:${name}:ready"
    GATE_RELEASE_KEY="${REDIS_PREFIX}gate:${name}:release"
    redis_cli del "$GATE_READY_KEY" "$GATE_RELEASE_KEY" >/dev/null
}

wait_for_gate_ready() {
    local expected=$1
    local length
    local attempt
    GATE_LAST_STATE="unknown"
    for attempt in {1..900}; do
        if length=$(redis_cli llen "$GATE_READY_KEY" 2>/dev/null); then
            GATE_LAST_STATE="llen=$length"
            if [[ $length =~ ^[0-9]+$ ]] && (( length >= expected )); then
                return 0
            fi
        else
            GATE_LAST_STATE="redis-error"
        fi
        sleep 0.1
    done
    return 1
}

release_gate() {
    local count=$1
    local index
    for index in $(seq 1 "$count"); do
        redis_cli rpush "$GATE_RELEASE_KEY" "release-$index" >/dev/null
    done
}

run_mix() {
    local directory=$ACTIVE_RESULT_ROOT/mix
    local index
    local detail
    mkdir -p "$directory"
    REQUEST_PIDS=()
    for index in $(seq 1 "$COUNT"); do
        start_request "$directory" "$index" "$BASE_URL/mix?id=$index&sleep=0.3"
    done
    wait_requests || true
    if detail=$(python3 "$SUITE_DIR/assert.py" mix "$directory" "$COUNT" "$CURRENT_ENV" 2>&1); then
        record "$(result_name mix)" PASS "APP_ENV=$CURRENT_ENV; 8 concurrent requests: $detail"
    else
        record "$(result_name mix)" ERROR "$detail"
    fi
}

run_session() {
    local round_one=$ACTIVE_RESULT_ROOT/session-round1
    local round_two=$ACTIVE_RESULT_ROOT/session-round2
    local gate_one=${RUN_ID}_${ACTIVE_PREFIX}session_one
    local gate_two=${RUN_ID}_${ACTIVE_PREFIX}session_two
    local index
    local jar
    local detail
    mkdir -p "$round_one" "$round_two"

    # Warm session/bootstrapping before the gated round so the gate measures
    # concurrency, not first-hit compilation.
    curl --silent --show-error --http1.1 --connect-timeout 5 --max-time "$HTTP_TIMEOUT" \
        "$BASE_URL/session?user=warmup" -o "$RESULT_ROOT/session-warmup.body" || true

    prepare_gate "$gate_one"
    REQUEST_PIDS=()
    for index in $(seq 1 "$COUNT"); do
        jar=$RESULT_ROOT/session-$index.cookie
        rm -f "$jar"
        start_request "$round_one" "$index" \
            "$BASE_URL/session?user=user$index&gate=$gate_one" -c "$jar"
    done
    if ! wait_for_gate_ready "$COUNT"; then
        stop_requests
        record "$(result_name session)" ERROR "APP_ENV=$CURRENT_ENV; round 1 did not reach its Redis gate (${GATE_LAST_STATE})"
        return
    fi
    release_gate "$COUNT"
    wait_requests || true

    prepare_gate "$gate_two"
    REQUEST_PIDS=()
    for index in $(seq 1 "$COUNT"); do
        jar=$RESULT_ROOT/session-$index.cookie
        start_request "$round_two" "$index" \
            "$BASE_URL/session?user=user$index&gate=$gate_two" -b "$jar" -c "$jar"
    done
    if ! wait_for_gate_ready "$COUNT"; then
        stop_requests
        record "$(result_name session)" ERROR "APP_ENV=$CURRENT_ENV; round 2 did not reach its Redis gate (${GATE_LAST_STATE})"
        return
    fi
    release_gate "$COUNT"
    wait_requests || true

    if detail=$(python3 "$SUITE_DIR/assert.py" session "$round_one" "$round_two" "$COUNT" "$CURRENT_ENV" 2>&1); then
        record "$(result_name session)" PASS "APP_ENV=$CURRENT_ENV; 8/8 own session IDs and data; second cookie round count=2: $detail"
    else
        record "$(result_name session)" ERROR "$detail"
    fi
}

run_auth() {
    local round_one=$ACTIVE_RESULT_ROOT/auth-round1
    local round_two=$ACTIVE_RESULT_ROOT/auth-round2
    local gate_one=${RUN_ID}_${ACTIVE_PREFIX}auth_one
    local gate_two=${RUN_ID}_${ACTIVE_PREFIX}auth_two
    local users=(alice alice alice alice bob bob bob bob)
    local index
    local user
    local jar
    local detail
    mkdir -p "$round_one" "$round_two"

    prepare_gate "$gate_one"
    REQUEST_PIDS=()
    for index in $(seq 1 "${#users[@]}"); do
        user=${users[$((index - 1))]}
        jar=$RESULT_ROOT/auth-$index.cookie
        rm -f "$jar"
        start_request "$round_one" "$index" \
            "$BASE_URL/me?gate=$gate_one" -u "$user:$user" -c "$jar"
    done
    if ! wait_for_gate_ready "${#users[@]}"; then
        stop_requests
        record "$(result_name stateful-auth)" ERROR "APP_ENV=$CURRENT_ENV; authenticated round 1 did not reach its Redis gate (${GATE_LAST_STATE})"
        return
    fi
    release_gate "${#users[@]}"
    wait_requests || true

    prepare_gate "$gate_two"
    REQUEST_PIDS=()
    for index in $(seq 1 "${#users[@]}"); do
        jar=$RESULT_ROOT/auth-$index.cookie
        start_request "$round_two" "$index" \
            "$BASE_URL/me?gate=$gate_two" -b "$jar" -c "$jar"
    done
    if ! wait_for_gate_ready "${#users[@]}"; then
        stop_requests
        record "$(result_name stateful-auth)" ERROR "APP_ENV=$CURRENT_ENV; authenticated round 2 did not reach its Redis gate (${GATE_LAST_STATE})"
        return
    fi
    release_gate "${#users[@]}"
    wait_requests || true

    if detail=$(python3 "$SUITE_DIR/assert.py" auth "$round_one" "$round_two" "$CURRENT_ENV" "${users[@]}" 2>&1); then
        record "$(result_name stateful-auth)" PASS "APP_ENV=$CURRENT_ENV; 4 alice + 4 bob; round 2 had no Authorization header: $detail"
    else
        record "$(result_name stateful-auth)" ERROR "$detail"
    fi
}

run_identity() {
    local directory=$ACTIVE_RESULT_ROOT/identity
    local gate=${RUN_ID}_${ACTIVE_PREFIX}identity
    local index
    local detail
    mkdir -p "$directory"
    prepare_gate "$gate"
    REQUEST_PIDS=()
    for index in $(seq 1 "$COUNT"); do
        start_request "$directory" "$index" "$BASE_URL/identity?id=$index&gate=$gate"
    done
    if ! wait_for_gate_ready "$COUNT"; then
        stop_requests
        record "$(result_name object-identity)" ERROR "APP_ENV=$CURRENT_ENV; identity requests did not reach their Redis gate"
        return
    fi
    release_gate "$COUNT"
    wait_requests || true
    if detail=$(python3 "$SUITE_DIR/assert.py" identity "$directory" "$COUNT" "$CURRENT_ENV" 2>&1); then
        record "$(result_name object-identity)" PASS "APP_ENV=$CURRENT_ENV; kernel, container, request, EntityManager and DBAL objects are distinct: $detail"
    else
        record "$(result_name object-identity)" ERROR "$detail"
    fi
}

run_negative_shared_includes() {
    local config=$RUN_ROOT/negative.conf
    local directory=$RESULT_ROOT/negative-shared-includes
    local first_code
    local second_code
    local log
    local first_json
    local second_body
    mkdir -p "$directory"
    stop_pid "$FPM_PID"
    FPM_PID=
    write_pool_config "$config" "$FPM_HTTP_PORT" "$FPM_FCGI_PORT" 0 fiber "$RUN_ROOT/negative-fpm.log"
    start_fpm "$config" 0 "$RUN_ROOT/negative-fpm.log"
    if ! wait_for_port "$FPM_HTTP_PORT"; then
        log=$(tail -20 "$RUN_ROOT/negative-fpm.log" 2>/dev/null || true)
        stop_pid "$FPM_PID"
        FPM_PID=
        record negative-shared-includes ERROR "negative-control pool did not start: $log"
        return
    fi

    curl --silent --show-error --http1.1 --connect-timeout 5 --max-time "$HTTP_TIMEOUT" \
        "$BASE_URL/identity" -o "$directory/first.body" -w '%{http_code}' \
        >"$directory/first.code" 2>"$directory/first.err" || true
    curl --silent --show-error --http1.1 --connect-timeout 5 --max-time "$HTTP_TIMEOUT" \
        "$BASE_URL/identity" -o "$directory/second.body" -w '%{http_code}' \
        >"$directory/second.code" 2>"$directory/second.err" || true
    sleep 0.5
    first_code=$(cat "$directory/first.code" 2>/dev/null || true)
    second_code=$(cat "$directory/second.code" 2>/dev/null || true)
    log=$(cat "$RUN_ROOT/negative-fpm.log" 2>/dev/null || true)
    first_json=$(cat "$directory/first.body" 2>/dev/null || true)
    second_body=$(cat "$directory/second.body" 2>/dev/null || true)
    stop_pid "$FPM_PID"
    FPM_PID=

    if [[ $first_code == 200 ]] \
        && grep -q 'kernel_oid' <<<"$first_json" \
        && grep -Eiq 'Cannot redeclare|ComposerAutoloaderInit' <<<"$second_body"; then
        record negative-shared-includes PASS "expected request-2 Composer redeclaration was reproduced without FPMNG_SHARED_INCLUDES (HTTP $first_code/$second_code; status is not the assertion)"
    else
        record negative-shared-includes ERROR "expected redeclaration was not reproduced (HTTP $first_code/$second_code); inspect $directory and $RUN_ROOT/negative-fpm.log"
    fi
}

run_unsupported_configuration() {
    local config=$RUN_ROOT/unsupported.conf
    local async_config=$RUN_ROOT/async.conf
    local output
    local async_output
    local unsupported_status=0
    local async_status=0
    local detail

    write_unsupported_config "$config" "$FPM_FCGI_PORT"
    if output=$("$FPMNG_BIN" "${FPM_OPTIONS[@]}" -t -y "$config" 2>&1); then
        unsupported_status=0
    else
        unsupported_status=$?
    fi

    write_async_config "$async_config" "$((FPM_FCGI_PORT + 1))"
    if async_output=$("$FPMNG_BIN" "${FPM_OPTIONS[@]}" -t -y "$async_config" 2>&1); then
        async_status=0
    else
        async_status=$?
    fi

    if [[ $unsupported_status -ne 0 ]] \
        && grep -Eiq 'fiber\.isolate_statics|not supported|unsupported' <<<"$output" \
        && [[ $async_status -ne 0 ]] \
        && grep -Eiq 'pool\.executor|async|disabled|not supported' <<<"$async_output"; then
        record unsupported-configuration PASS "classic rejected fiber.isolate_statics and async executor was rejected"
    else
        detail="classic status=$unsupported_status, async status=$async_status; classic output: $output; async output: $async_output"
        record unsupported-configuration ERROR "$detail"
    fi
}

setup_binary() {
    local marker
    local missing=
    local modules

    if [[ -z $FPMNG_BIN ]]; then
        mark_not_measured "FPMNG_BIN is required; no framework result is claimed"
        return 1
    fi
    if [[ ! -x $FPMNG_BIN ]]; then
        mark_not_measured "FPMNG_BIN is not an executable file: $FPMNG_BIN"
        return 1
    fi
    FPMNG_BIN=$(cd -- "$(dirname -- "$FPMNG_BIN")" && pwd)/$(basename -- "$FPMNG_BIN")
    BINARY_SHA=$(sha256_of "$FPMNG_BIN")
    BINARY_VERSION=$("$FPMNG_BIN" -v 2>&1 | tr '\n' ' ')
    BINARY_VERSION=${BINARY_VERSION:0:240}
    SOURCE_COMMIT=${FPMNG_SOURCE_COMMIT:-$(git -C "$REPO_DIR" rev-parse HEAD 2>/dev/null || echo unknown)}

    if [[ -n ${FPMNG_EXPECTED_SHA256:-} && $BINARY_SHA != "$FPMNG_EXPECTED_SHA256" ]]; then
        mark_not_measured "binary SHA-256 $BINARY_SHA does not match FPMNG_EXPECTED_SHA256"
        return 1
    fi

    strings "$FPMNG_BIN" > "$RUN_ROOT/binary.strings" 2>/dev/null || true
    for marker in FPMNG_SHARED_INCLUDES http.front_controller fiber.revalidate_freq fiber.isolate_statics; do
        if ! grep -Fq "$marker" "$RUN_ROOT/binary.strings"; then
            missing+=" $marker"
        fi
    done
    if [[ -n $missing ]]; then
        mark_not_measured "binary fingerprint $BINARY_SHA is missing required php-fpm-ng markers:$missing; likely the wrong/stale build"
        return 1
    fi

    FPM_OPTIONS=(-n)
    if [[ -n ${FPMNG_REDIS_EXTENSION:-} ]]; then
        if [[ ! -f $FPMNG_REDIS_EXTENSION ]]; then
            mark_not_measured "FPMNG_REDIS_EXTENSION does not exist: $FPMNG_REDIS_EXTENSION"
            return 1
        fi
        FPM_OPTIONS+=(-d "extension=$FPMNG_REDIS_EXTENSION")
    fi
    modules=$("$FPMNG_BIN" "${FPM_OPTIONS[@]}" -m 2>&1)
    if ! grep -qx 'PDO' <<<"$modules" || ! grep -qx 'pdo_mysql' <<<"$modules"; then
        mark_not_measured "tested FPM binary does not provide PDO and pdo_mysql"
        return 1
    fi
    if ! grep -qx 'session' <<<"$modules"; then
        mark_not_measured "tested FPM binary does not provide ext/session"
        return 1
    fi
    if grep -qx 'redis' <<<"$modules"; then
        REDIS_CLIENT=phpredis
    fi
    return 0
}

setup_composer() {
    local output
    local php_bin=${PHP_BIN:-php}

    rm -rf "$APP_DIR"
    mkdir -p "$APP_DIR"
    tar -C "$SUITE_DIR" --exclude=./vendor --exclude=./var --exclude=./.runs \
        --exclude=./__pycache__ -cf - . | tar -C "$APP_DIR" -xf -
    if [[ -n ${COMPOSER_BIN:-} ]]; then
        COMPOSER=("$COMPOSER_BIN")
    elif command_exists composer; then
        COMPOSER=(composer)
    elif [[ -x $php_bin && -n ${COMPOSER_PHAR:-} && -f $COMPOSER_PHAR ]]; then
        COMPOSER=("$php_bin" "$COMPOSER_PHAR")
    else
        mark_not_measured "Composer is unavailable; set COMPOSER_BIN or PHP_BIN plus COMPOSER_PHAR"
        return 1
    fi

    if ! output=$(COMPOSER_HOME="$RUN_ROOT/composer-home" "${COMPOSER[@]}" install \
        --working-dir="$APP_DIR" --no-interaction --no-progress --prefer-dist --no-scripts 2>&1); then
        mark_not_measured "composer install could not run: $output"
        return 1
    fi
    return 0
}

setup_services() {
    local attempt
    local ready=0

    if [[ $SERVICE_MODE != docker && $SERVICE_MODE != external ]]; then
        mark_not_measured "SERVICE_MODE must be docker or external"
        return 1
    fi

    if [[ $SERVICE_MODE == docker ]]; then
        if ! command_exists docker || ! docker compose version >/dev/null 2>&1; then
            mark_not_measured "Docker Compose is required in SERVICE_MODE=docker; use SERVICE_MODE=external with explicit service endpoints"
            return 1
        fi
        MYSQL_PORT=$(choose_free_port "$MYSQL_PORT")
        REDIS_PORT=$(choose_free_port "$REDIS_PORT")
        export MYSQL_PORT REDIS_PORT MYSQL_ROOT_PASSWORD
        COMPOSE_PROJECT="fpmng-symfony-$(printf '%s' "$RUN_ID" | tr 'A-Z:' 'a-z--')"
        COMPOSE=(docker compose -f "$SUITE_DIR/compose.yaml" -p "$COMPOSE_PROJECT")
        if ! "${COMPOSE[@]}" up -d >"$RUN_ROOT/compose.log" 2>&1; then
            mark_not_measured "Docker Compose could not start MySQL and Redis; see $RUN_ROOT/compose.log"
            return 1
        fi
        DOCKER_STARTED=1
        MYSQL_HOST=127.0.0.1
        REDIS_HOST=127.0.0.1
        MYSQL_ADMIN_USER=root
        MYSQL_ADMIN_PASSWORD=$MYSQL_ROOT_PASSWORD
        MYSQL_APP_USER=root
        MYSQL_APP_PASSWORD=$MYSQL_ROOT_PASSWORD
        for attempt in {1..90}; do
            if "${COMPOSE[@]}" exec -T mysql mysqladmin ping --protocol=tcp -h 127.0.0.1 \
                -uroot -p"$MYSQL_ROOT_PASSWORD" >/dev/null 2>&1 \
                && "${COMPOSE[@]}" exec -T redis redis-cli ping >/dev/null 2>&1; then
                ready=1
                break
            fi
            sleep 1
        done
        if [[ $ready -ne 1 ]]; then
            mark_not_measured "Docker services did not become ready; see $RUN_ROOT/compose.log"
            return 1
        fi
    else
        MYSQL_HOST=${MYSQL_HOST:-127.0.0.1}
        REDIS_HOST=${REDIS_HOST:-127.0.0.1}
        MYSQL_APP_USER=${MYSQL_USER:-root}
        MYSQL_APP_PASSWORD=${MYSQL_PASSWORD:-}
        MYSQL_ADMIN_USER=${MYSQL_ADMIN_USER:-$MYSQL_APP_USER}
        MYSQL_ADMIN_PASSWORD=${MYSQL_ADMIN_PASSWORD:-$MYSQL_APP_PASSWORD}
        if ! mysql_admin 'SELECT 1' >/dev/null 2>&1; then
            mark_not_measured "external MySQL is unavailable or the admin credentials cannot create a database"
            return 1
        fi
        if ! redis_cli ping >/dev/null 2>&1; then
            mark_not_measured "external Redis is unavailable at $REDIS_HOST:$REDIS_PORT database $REDIS_DB"
            return 1
        fi
    fi

    DB_NAME="fpmng_symfony_${RUN_ID//[^A-Za-z0-9]/_}"
    if ! mysql_admin "CREATE DATABASE $DB_NAME CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;" \
        >"$RUN_ROOT/mysql-create.log" 2>&1; then
        mark_not_measured "could not create private MySQL database $DB_NAME; see $RUN_ROOT/mysql-create.log"
        return 1
    fi
    DB_CREATED=1
    if [[ $DOCKER_STARTED -eq 0 && ${MYSQL_ADMIN_SUDO:-0} -eq 1 ]]; then
        if [[ ! $MYSQL_APP_USER =~ ^[A-Za-z0-9_]+$ ]]; then
            mark_not_measured "MYSQL_USER must contain only letters, digits, and underscores when MYSQL_ADMIN_SUDO=1"
            return 1
        fi
        if ! mysql_admin "GRANT ALL PRIVILEGES ON $DB_NAME.* TO '$MYSQL_APP_USER'@'%'; GRANT ALL PRIVILEGES ON $DB_NAME.* TO '$MYSQL_APP_USER'@'localhost';" \
            >"$RUN_ROOT/mysql-grant.log" 2>&1; then
            mark_not_measured "could not grant the application user access to private MySQL database $DB_NAME; see $RUN_ROOT/mysql-grant.log"
            return 1
        fi
    fi
    if ! mysql_admin "USE $DB_NAME; CREATE TABLE items (id INT PRIMARY KEY, label VARCHAR(64) NOT NULL); INSERT INTO items (id, label) VALUES (1, 'item-1'), (2, 'item-2'), (3, 'item-3'), (4, 'item-4'), (5, 'item-5'), (6, 'item-6'), (7, 'item-7'), (8, 'item-8');" \
        >"$RUN_ROOT/mysql-schema.log" 2>&1; then
        mark_not_measured "could not initialize private MySQL database $DB_NAME; see $RUN_ROOT/mysql-schema.log"
        return 1
    fi
    return 0
}

write_runtime_environment() {
    local mysql_user_encoded
    local mysql_password_encoded
    local cookie_suffix

    mysql_user_encoded=$(url_encode "$MYSQL_APP_USER")
    mysql_password_encoded=$(url_encode "$MYSQL_APP_PASSWORD")
    cookie_suffix=${RUN_ID//[^A-Za-z0-9]/}
    SESSION_COOKIE_NAME="FPMNGSESSID_${cookie_suffix:0:32}"
    REDIS_PREFIX="fpmng:symfony:${RUN_ID}:"
    export APP_ENV=${APP_ENV:-dev}
    export APP_DEBUG=${APP_DEBUG:-1}
    export APP_SECRET=${APP_SECRET:-fpmng-symfony-probe-secret}
    export DATABASE_URL="mysql://${mysql_user_encoded}:${mysql_password_encoded}@${MYSQL_HOST}:${MYSQL_PORT}/${DB_NAME}?serverVersion=8.4&charset=utf8mb4"
    export REDIS_DSN="redis://${REDIS_HOST}:${REDIS_PORT}/${REDIS_DB}"
    export REDIS_PREFIX SESSION_COOKIE_NAME
    export FPMNG_SHARED_INCLUDES=1
}

start_pool() {
    # start_pool CHILDREN REVALIDATE_FREQ -> starts the verified binary with a
    # fresh pool config for the currently exported runtime environment.
    local children=$1
    local revalidate_freq=${2:-0}
    START_POOL_SEQ=$((START_POOL_SEQ + 1))
    local tag="${CURRENT_ENV}-${children}-${revalidate_freq}-${START_POOL_SEQ}"
    local config=$RUN_ROOT/fpm-pool-$tag.conf
    FPM_HTTP_PORT=$(choose_free_port "$FPM_HTTP_PORT")
    FPM_FCGI_PORT=$(choose_free_port "$FPM_FCGI_PORT")
    BASE_URL="http://127.0.0.1:$FPM_HTTP_PORT"
    write_pool_config "$config" "$FPM_HTTP_PORT" "$FPM_FCGI_PORT" 1 fiber \
        "$RUN_ROOT/fpm-pool-$tag.log" "$children" "$revalidate_freq"
    start_fpm "$config" 1 "$RUN_ROOT/fpm-pool-$tag.log"
    if ! wait_for_port "$FPM_HTTP_PORT" || ! wait_for_health; then
        local log
        log=$(tail -40 "$RUN_ROOT/fpm.log" 2>/dev/null || true)
        stop_pid "$FPM_PID"
        FPM_PID=
        echo "POOL_START_FAILED: $log" >&2
        return 1
    fi
    return 0
}

run_core_scenarios() {
    run_mix
    run_session
    run_auth
    run_identity
}

run_suite() {
    # run_suite PREFIX ENV CHILDREN -> run the four core scenarios under one
    # dedicated pool so APP_ENV and pm.max_children are isolated per suite.
    local prefix=$1
    local environment=$2
    local children=$3
    ACTIVE_PREFIX=$prefix
    ACTIVE_RESULT_ROOT=$RESULT_ROOT/${prefix:-dev}
    CURRENT_ENV=$environment
    mkdir -p "$ACTIVE_RESULT_ROOT"
    export APP_ENV=$environment
    export APP_DEBUG=$([ "$environment" = prod ] && echo 0 || echo 1)
    if ! start_pool "$children"; then
        local name
        for name in mix session stateful-auth object-identity; do
            record "${prefix}${name}" "NOT MEASURED" "APP_ENV=$environment; the verified FPM binary did not start the Symfony pool (children=$children)"
        done
        return 1
    fi
    # Each scenario gets a fresh pool: a request that stalls inside one
    # scenario (e.g. a fiber blocked in a gate BLPOP) must not poison the
    # measurements of the scenarios that follow it.
    run_mix
    stop_pid "$FPM_PID"; FPM_PID=
    start_pool "$children" || return 1
    run_session
    stop_pid "$FPM_PID"; FPM_PID=
    start_pool "$children" || return 1
    run_auth
    stop_pid "$FPM_PID"; FPM_PID=
    start_pool "$children" || return 1
    run_identity
    stop_pid "$FPM_PID"; FPM_PID=
}

run_framework_twig() {
    local directory=$RESULT_ROOT/framework-twig
    local users=(alice bob alice bob)
    local index
    local user
    local detail
    mkdir -p "$directory"
    # Warm the endpoint (compile Twig, form, validator classes) before the
    # gated window so the gate measures concurrency, not cold compilation.
    curl --silent --show-error --http1.1 --connect-timeout 5 --max-time "$HTTP_TIMEOUT" \
        "$BASE_URL/twig?marker=warmup&value=warmup" -u 'alice:alice' \
        -o "$directory/warmup.body" -w '%{http_code}' >"$directory/warmup.code" || true
    prepare_gate "${RUN_ID}_twig"
    REQUEST_PIDS=()
    for index in $(seq 1 "${#users[@]}"); do
        user=${users[$((index - 1))]}
        start_request "$directory" "$index" \
            "$BASE_URL/twig?marker=marker-$index&value=value-$index&gate=${RUN_ID}_twig" \
            -u "$user:$user"
    done
    if wait_for_gate_ready "${#users[@]}"; then
        release_gate "${#users[@]}"
        GATE_RELEASE_CHECK="release=$(redis_cli llen "$GATE_RELEASE_KEY" 2>/dev/null) ready=$(redis_cli llen "$GATE_READY_KEY" 2>/dev/null)"
    else
        record framework-twig ERROR "twig gate: only ${GATE_LAST_STATE} of ${#users[@]} ready after the wait window"
        wait_requests || true
        return
    fi
    wait_requests || true
    if detail=$(python3 "$SUITE_DIR/assert.py" twig "$directory" "$CURRENT_ENV" "${users[@]}" 2>&1); then
        record framework-twig PASS "Twig renderView + custom extension gate; app.user and request data rendered per user: $detail"
    else
        record framework-twig ERROR "$GATE_RELEASE_CHECK; $detail"
    fi
}

run_framework_form_validation() {
    local directory=$RESULT_ROOT/framework-form-validation
    local names=(alpha '' beta '' gamma)
    local index
    local name
    local encoded
    local detail
    mkdir -p "$directory"
    curl --silent --show-error --http1.1 --connect-timeout 5 --max-time "$HTTP_TIMEOUT" \
        "$BASE_URL/form?name=warmup" -o "$directory/warmup.body" -w '%{http_code}' \
        >"$directory/warmup.code" || true
    prepare_gate "${RUN_ID}_form"
    for index in $(seq 1 "${#names[@]}"); do
        name=${names[$((index - 1))]}
        encoded=$(url_encode "$name")
        start_request "$directory" "$index" "$BASE_URL/form?name=$encoded&gate=${RUN_ID}_form"
    done
    if wait_for_gate_ready "${#names[@]}"; then
        release_gate "${#names[@]}"
    else
        record framework-form-validation ERROR "form gate: only ${GATE_LAST_STATE} of ${#names[@]} ready after the wait window"
        wait_requests || true
        return
    fi
    wait_requests || true
    if detail=$(python3 "$SUITE_DIR/assert.py" form "$directory" "$CURRENT_ENV" "${names[@]}" 2>&1); then
        record framework-form-validation PASS "form submit + NotBlank validator; valid/invalid split asserted on response data: $detail"
    else
        record framework-form-validation ERROR "$detail"
    fi
}

run_framework_messenger() {
    local directory=$RESULT_ROOT/framework-messenger
    local values=(msg-one msg-two msg-three msg-four)
    local index
    local value
    local detail
    mkdir -p "$directory"
    curl --silent --show-error --http1.1 --connect-timeout 5 --max-time "$HTTP_TIMEOUT" \
        "$BASE_URL/messenger?value=warmup" -u 'alice:alice' -o "$directory/warmup.body" \
        -w '%{http_code}' >"$directory/warmup.code" || true
    prepare_gate "${RUN_ID}_messenger"
    for index in $(seq 1 "${#values[@]}"); do
        value=${values[$((index - 1))]}
        start_request "$directory" "$index" "$BASE_URL/messenger?value=$value&gate=${RUN_ID}_messenger" \
            -u "alice:alice"
    done
    if wait_for_gate_ready "${#values[@]}"; then
        release_gate "${#values[@]}"
    else
        record framework-messenger ERROR "messenger gate: only ${GATE_LAST_STATE} of ${#values[@]} ready after the wait window"
        wait_requests || true
        return
    fi
    wait_requests || true
    if detail=$(python3 "$SUITE_DIR/assert.py" messenger "$directory" "$CURRENT_ENV" "${values[@]}" 2>&1); then
        record framework-messenger PASS "sync Messenger bus dispatch + handler result asserted on HandledStamp data: $detail"
    else
        record framework-messenger ERROR "$detail"
    fi
}

run_rss_stability() {
    local directory=$RESULT_ROOT/rss-stability
    local rounds=200
    local max_growth_kb=6144
    local index
    local detail
    mkdir -p "$directory"
    for index in $(seq 1 "$rounds"); do
        start_request "$directory" "$index" "$BASE_URL/identity?id=$index"
        wait_requests || true
    done
    if detail=$(python3 "$SUITE_DIR/assert.py" rss "$directory" "$rounds" "$CURRENT_ENV" "$max_growth_kb" 2>&1); then
        record rss-stability PASS "$rounds sequential requests in one worker, RSS growth limit ${max_growth_kb} KiB: $detail"
    else
        record rss-stability ERROR "$detail"
    fi
}

run_fiber_revalidate() {
    local directory=$RESULT_ROOT/fiber-revalidate
    local marker_file=$APP_DIR/src/Probe/DeployMarker.php
    mkdir -p "$directory"
    CURRENT_ENV=dev
    ACTIVE_PREFIX=
    ACTIVE_RESULT_ROOT=$RESULT_ROOT
    export APP_ENV=dev
    export APP_DEBUG=1
    if ! start_pool 1 1; then
        record fiber-revalidate "NOT MEASURED" "the verified FPM binary did not start the revalidation pool"
        return 1
    fi

    curl --silent --show-error --http1.1 --connect-timeout 5 --max-time "$HTTP_TIMEOUT" \
        "$BASE_URL/deploy" -o "$directory/first.body" -w '%{http_code}' >"$directory/first.code" || true
    sed -i '' 's/release-1/release-2/' "$marker_file" 2>/dev/null \
        || sed -i 's/release-1/release-2/' "$marker_file"
    sleep 2
    curl --silent --show-error --http1.1 --connect-timeout 5 --max-time "$HTTP_TIMEOUT" \
        "$BASE_URL/deploy" -o "$directory/immediate.body" -w '%{http_code}' >"$directory/immediate.code" || true
    sleep 2
    curl --silent --show-error --http1.1 --connect-timeout 5 --max-time "$HTTP_TIMEOUT" \
        "$BASE_URL/deploy" -o "$directory/eventual.body" -w '%{http_code}' >"$directory/eventual.code" || true
    stop_pid "$FPM_PID"
    FPM_PID=

    if detail=$(python3 "$SUITE_DIR/assert.py" deploy "$directory" dev 2>&1); then
        record fiber-revalidate PASS "deploy marker changed on disk with fiber.revalidate_freq=1; updated code served: $detail"
    else
        record fiber-revalidate ERROR "$detail"
    fi
}

run_framework_surface() {
    if ! start_pool 1; then
        record framework-twig "NOT MEASURED" "the verified FPM binary did not start the framework-surface pool"
        record framework-form-validation "NOT MEASURED" "the verified FPM binary did not start the framework-surface pool"
        record framework-messenger "NOT MEASURED" "the verified FPM binary did not start the framework-surface pool"
        record rss-stability "NOT MEASURED" "the verified FPM binary did not start the framework-surface pool"
        return 1
    fi
    run_framework_twig
    run_framework_form_validation
    run_framework_messenger
    run_rss_stability
    stop_pid "$FPM_PID"
    FPM_PID=
}

run_app_env_comparison() {
    local dev_status=${SCENARIO_STATUS[$(scenario_index mix)]:-}
    local prod_status=${SCENARIO_STATUS[$(scenario_index prod-mix)]:-}
    if [[ $dev_status == PASS && $prod_status == PASS ]]; then
        record app-env-comparison PASS "APP_ENV=dev and APP_ENV=prod pools both served the concurrent mix with the correct kernel environment reported in each response"
    else
        record app-env-comparison ERROR "dev mix=$dev_status, prod mix=$prod_status; both environments must pass mix to claim the comparison"
    fi
}

run_pm_max_children() {
    local statuses=()
    local name
    local status
    for name in pm2-mix pm2-session pm2-stateful-auth pm2-object-identity; do
        statuses+=("${SCENARIO_STATUS[$(scenario_index "$name")]:-}")
    done
    local failed=0
    for status in "${statuses[@]}"; do
        [[ $status == PASS ]] || failed=1
    done
    if (( failed == 0 )); then
        record pm-max-children PASS "pm.max_children=2 pool served all concurrent scenarios (mix/session/auth/identity): ${statuses[*]}"
    else
        record pm-max-children ERROR "pm.max_children=2 pool did not serve all concurrent scenarios: ${statuses[*]}"
    fi
}

run_main_suite() {
    run_suite "" dev 1
    run_suite prod- prod 1
    run_suite pm2- dev 2
    CURRENT_ENV=dev
    ACTIVE_PREFIX=
    ACTIVE_RESULT_ROOT=$RESULT_ROOT
    run_framework_surface
    run_fiber_revalidate
    run_app_env_comparison
    run_pm_max_children
}

write_report() {
    local report=$RUN_ROOT/report.md
    local pass=0
    local error=0
    local not_measured=0
    local index
    local status
    {
        echo '# Symfony framework probe results'
        echo
        echo "- Run: $RUN_ID"
        echo "- Repository source commit: $SOURCE_COMMIT"
        echo "- php-fpm-ng: $FPMNG_BIN"
        echo "- Binary SHA-256: $BINARY_SHA"
        echo "- Binary version: $BINARY_VERSION"
        echo "- Binary markers: $BINARY_MARKERS"
        echo "- Redis client: $REDIS_CLIENT"
        echo "- APP_ENV: ${APP_ENV:-not-set}"
        echo
        echo '| Scenario | Result | Detail |'
        echo '|---|---|---|'
        for index in "${!SCENARIO_NAMES[@]}"; do
            status=${SCENARIO_STATUS[$index]:-NOT MEASURED}
            printf '| `%s` | **%s** | %s |\n' "${SCENARIO_NAMES[$index]}" "$status" "${SCENARIO_DETAIL[$index]:-not run}"
            case $status in
                PASS) pass=$((pass + 1)) ;;
                ERROR|FAIL) error=$((error + 1)) ;;
                "NOT MEASURED") not_measured=$((not_measured + 1)) ;;
            esac
        done
        echo
        echo "Summary: PASS=$pass ERROR=$error NOT MEASURED=$not_measured"
        echo
        echo 'The negative-control scenarios are expected to exercise rejected or broken configurations. They are PASS only when the expected failure is observed; they do not weaken the positive data assertions.'
    } > "$report"
    cat "$report"
    printf 'REPORT=%s\n' "$report"
    if (( error > 0 )); then
        return 1
    fi
    if (( pass == 0 )); then
        return 2
    fi
    return 0
}

main() {
    local setup_failed=0

    for command in curl nc python3 strings; do
        if ! command_exists "$command"; then
            mark_not_measured "required command is missing: $command"
            setup_failed=1
        fi
    done
    if (( setup_failed )); then
        write_report || true
        return 2
    fi

    if ! setup_binary; then
        write_report || true
        return 2
    fi
    if ! setup_composer; then
        write_report || true
        return 2
    fi
    if ! setup_services; then
        write_report || true
        return 2
    fi
    write_runtime_environment

    run_main_suite || true
    run_negative_shared_includes || true
    run_unsupported_configuration || true

    write_report
}

main "$@"
