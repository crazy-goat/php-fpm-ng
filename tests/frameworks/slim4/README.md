# Slim 4 fiber probe

This directory contains the probe application for task 026. It is deliberately
small and does not add Slim-specific code to `sapi/fpmng/`.

## Versions

- Slim: `4.15.3`
- PSR-7 implementation: `slim/psr7` `1.8.0`
- PHP-DI: `7.1.1`
- Redis client: `predis/predis` `3.6.0`

The versions are pinned in `composer.json` and `composer.lock`.

## What the probe tests

`public/index.php` is the normal Slim entry script. It uses
`require ../vendor/autoload.php`, not a hand-written shared-includes workaround.
The pool sets `FPMNG_SHARED_INCLUDES=1`, `pool.executor = fiber` and
`pm.max_children = 1`. Requests use bare paths such as `/health`; the current
HTTP gateway's default `/index.php` front-controller fallback invokes the stock
entry script without adding `/index.php` to Slim's route path.

`bin/run.php` runs eight concurrent requests and reports every scenario as
`PASS`, `ERROR`, or `NOT MEASURED`. An `ERROR` makes the command fail, but the
scenario remains in the runner and in the result matrix. It does not turn a
failed assertion into a skip.

The current measured scenarios are:

- MySQL and Redis in one request;
- two session rounds with per-request cookies;
- session-backed authenticated identity;
- app, request, route-collector and container identity;
- large PSR-7 request bodies;
- PSR-7 response streams;
- middleware state;
- error middleware;
- repeated requests through the stock entry script with shared includes;
- PHP-DI container identity and service state;
- Slim route-cache routing and cache-file stability.

The PHP-DI and route-cache scenarios are mode-specific. If their mode is not
selected, the runner reports them as `NOT MEASURED`; when selected, an assertion
failure is reported as `ERROR`, never as a skip.

## Running

The command needs a built `php-fpm-ng`, PHP CLI with `PDO_MYSQL` and cURL, and
MySQL/Redis. `SERVICE_MODE` selects where those come from:

- `docker` (the default): the runner starts a private MySQL and Redis via
  `compose.yaml` — the same pinned images (`mysql:8.4.6`,
  `redis:7.4.2-alpine`) as `tests/frameworks/symfony/compose.yaml` — on free
  host ports, creates a per-run database, and tears the whole compose
  project down (`down --volumes --remove-orphans`) on exit. Nothing needs to
  be pre-provisioned except Docker itself.
- `external`: the previous behaviour — point at already-running services
  (e.g. the shared test box) via the `SLIM_DB_*` / `SLIM_REDIS_*` variables.
  Use a separate Redis database (`2` by default), and never run `FLUSHDB` or
  `FLUSHALL`.

```sh
composer install --no-interaction --prefer-dist
FPMNG=/path/to/php-fpm-ng \
PHP=/path/to/php \
./bin/run.sh
```

To point at already-running services instead of Docker Compose:

```sh
composer install --no-interaction --prefer-dist
SERVICE_MODE=external \
SLIM_DB_HOST=127.0.0.1 SLIM_REDIS_HOST=127.0.0.1 \
FPMNG=/path/to/php-fpm-ng \
PHP=/path/to/php \
./bin/run.sh
```

To measure the PHP-DI and route-cache rows, select their modes explicitly (they
can be combined):

```sh
SLIM_CONTAINER=php-di \
SLIM_ROUTE_CACHE=1 \
FPMNG=/path/to/php-fpm-ng \
PHP=/path/to/php \
./bin/run.sh
```

PHP-DI builds a fresh `DI\ContainerBuilder` container per request and exposes a
request-local identity service through Slim. Route-cache mode uses
`$RUN_DIR/route-cache.php`; readiness warms it once, and the runner compares its
cache fingerprint before and after concurrent routing/data requests.

The default ports are `22625` (FastCGI) and `22626` (HTTP); `choose_free_port`
bumps past whatever is already bound rather than failing, so a busy machine
or a parallel run does not need manual port bookkeeping. Override `HTTP_PORT`,
`FCGI_PORT`, `RUN_DIR`, `SERVICE_MODE`, `MYSQL_PORT`, `REDIS_PORT`, and the
`SLIM_DB_*` / `SLIM_REDIS_*` environment variables when the defaults need to
change. The script only stops the master process recorded in its own
`RUN_DIR`, and in `SERVICE_MODE=docker` also tears down its own compose
project on exit.

The runner exits with status 1 when any measured scenario reports `ERROR`.
The test code and the failing scenario must remain in the tree until the
failure has been understood and either fixed or documented as an intended
limitation.
