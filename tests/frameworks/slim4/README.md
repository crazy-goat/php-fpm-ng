# Slim 4 fiber probe

This directory contains the probe application for task 026. It is deliberately
small and does not add Slim-specific code to `sapi/fpmng/`.

## Versions

- Slim: `4.15.3`
- PSR-7 implementation: `slim/psr7` `1.8.0`
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
- repeated requests through the stock entry script with shared includes.

The PHP-DI container variant and Slim route-cache mode are present in the
matrix as `NOT MEASURED`; they are not silently treated as passing.

## Running

The command needs a built `php-fpm-ng`, PHP CLI with `PDO_MYSQL` and cURL, a
MySQL database account that can create the private `slim4` database, and Redis.
The defaults are the shared test-box services on localhost. Use a separate
Redis database (`2` by default), and do not run `FLUSHDB` or `FLUSHALL`.

```sh
composer install --no-interaction --prefer-dist
FPMNG=/path/to/php-fpm-ng \
PHP=/path/to/php \
./bin/run.sh
```

The default ports are `22625` (FastCGI) and `22626` (HTTP). The runner uses
bare route paths and therefore requires the HTTP gateway's front-controller
fallback. Override `HTTP_PORT`, `FCGI_PORT`, `RUN_DIR`, and
the `SLIM_DB_*` / `SLIM_REDIS_*` environment variables when the defaults are
occupied. The script only stops
the master process recorded in its own `RUN_DIR`.

The runner exits with status 1 when any measured scenario reports `ERROR`.
The test code and the failing scenario must remain in the tree until the
failure has been understood and either fixed or documented as an intended
limitation.
