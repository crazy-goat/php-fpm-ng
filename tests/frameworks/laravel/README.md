# Laravel 13 fiber probe

This directory contains the repository-owned Laravel fixture and runner for
Task 025. It deliberately adds no Laravel-specific code to `sapi/fpmng/`.

## Pinned versions

- Laravel framework: `13.30.1`
- Predis fallback package: `3.6.0` (locked and available for an explicit
  `REDIS_CLIENT=predis` run)
- The reproduced test-box run uses phpredis `6.3.0RC1`, loaded as an explicit
  PHP extension because the test-box PHP build does not compile phpredis in.
- The runner records the exact PHP-FPM-NG version, SHA-256 and distinctive
  `fiber.isolate_statics` / `FPMNG_SHARED_INCLUDES` strings before measuring.

`composer.lock` is part of this fixture. `composer install --no-dev` is the
only dependency installation step; no copy of the test-box's hand-built
`~/rd/apps/laravel` application is used.

## Probe coverage

The configured run checks response data and object identities for:

- concurrent Redis-backed `/session` requests, including a second cookie round;
- the documented empty-static-list negative control for `/session`;
- authenticated `/login` followed by concurrent `/me` requests;
- MySQL, Redis, Laravel cache and Eloquent data in one concurrent request;
- facade root, application, container and request identity;
- Eloquent's connection resolver after a real blocking MySQL suspension;
- middleware request state and `terminate()` attribution through Redis;
- CSRF tokens and cross-session rejection;
- validation errors flashed into the correct session;
- synchronous queue dispatch and synchronous broadcasting.

The configured pool uses this versioned list for Laravel 13.30.1:

```ini
fiber.isolate_statics = Illuminate\\Container\\Container::instance,Illuminate\\Support\\Facades\\Facade::app,Illuminate\\Support\\Facades\\Facade::resolvedInstance,Illuminate\\Database\\Eloquent\\Model::resolver
```

`Model::$resolver` was added by the repository-owned Eloquent probe: with only
the three previously documented entries, concurrent Eloquent queries returned
HTTP 500 with `Cannot execute queries while other unbuffered queries are
active`; adding that fourth entry made the Eloquent and mixed DB/Cache/Redis
scenarios pass. This is a measured configuration requirement, not Laravel code
added to fpm-ng.

The empty-list run repeats every implemented scenario, including `/session`,
`/me`, the mixed DB/Cache/Redis/Eloquent probe, middleware, CSRF, validation,
queue and broadcasting. A wrong HTTP-200 payload is an `ERROR`, not a pass. The
negative control intentionally reports `ERROR` for scenarios that need the
list; the failing assertion remains in the runner and the overall command exits
non-zero when it does. A control may legitimately `PASS` when that scenario
does not exercise one of the isolated statics. An incomplete list can return
HTTP 200 with another request's data, so
this configuration must be re-verified after every Laravel minor-version
upgrade.

The remaining matrix items are printed as `NOT MEASURED` rather than being
silently skipped or counted as passes.

## Dedicated test-box run

The runner is designed for a private directory and private resources:

- application/run directory: `/home/piotr/rd/tasks/025-laravel-tests`;
- FastCGI port: `22725`;
- HTTP port: `22726`;
- MySQL database: `laravel025`;
- Redis database: `3`.

It only terminates the master PID in its own run directory. It never sends a
process-name kill and never issues `FLUSHDB` or `FLUSHALL`. Do not point it at
the existing `/home/piotr/rd/apps/laravel` tree.

The single runner command provisions the locked dependencies when
`vendor/autoload.php` is absent. It uses Composer `2.10.3` from a pinned URL
and checksum when no `composer` executable is available:

```sh
cd tests/frameworks/laravel
PHP=/path/to/php \
FPMNG=/path/to/php-fpm-ng \
REDIS_CLIENT=phpredis \
REDIS_EXTENSION=/path/to/redis.so \
./bin/run.sh
```

To provision without starting a pool, run `PHP=/path/to/php ./bin/provision.sh`.
The PHP CLI needs the extensions required by the locked Laravel dependencies;
the runner also needs cURL for its concurrent HTTP client.

`REDIS_CLIENT=predis` is an explicit alternative that does not load a
phpredis extension. The test-box evidence in Task 025 uses `phpredis`; a run
with the alternative client must be reported as a different measurement.

`RUN_DIR`, `FCGI_PORT`, `HTTP_PORT`, `LARAVEL_DB_*`, `LARAVEL_REDIS_*`, `PHP`,
`FPMNG`, and `REDIS_EXTENSION` are overridable. The runner defaults to the
ports and resources above so parallel framework tracks do not share them.
