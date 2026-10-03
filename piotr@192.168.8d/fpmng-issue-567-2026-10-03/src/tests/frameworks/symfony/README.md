# Symfony fiber probe

This directory owns the Symfony application and the runner for task 024. It is
an API-only Symfony skeleton with Doctrine ORM, a Redis cache pool, a Redis
session handler, and the stateful `http_basic` firewall. The front controller is
hand-written and does not load `symfony/runtime`; this is required when
`FPMNG_SHARED_INCLUDES=1` is used.

## Pinned application

The lock file is committed. The relevant direct versions are:

| Dependency | Version |
|---|---:|
| Symfony components | 8.1.6 |
| Symfony Flex | 2.11.0 |
| Symfony ORM pack (unpacked by Flex) | 2.7.0 |
| Doctrine ORM | 3.6.8 |
| Doctrine DBAL | 4.4.4 (locked transitively) |
| DoctrineBundle | 3.3.1 |
| Doctrine MigrationsBundle (unpacked by Flex) | 4.0.1 |
| Predis | 3.6.0 |

Predis is included so the probe can run with a php-fpm-ng build that does not
load phpredis. If `FPMNG_REDIS_EXTENSION` is supplied and the binary loads it,
the report records `phpredis` instead. The reference measurement in
`docs/frameworks.md` used phpredis; a Predis result is identified as such and
is not silently presented as that measurement.

## Run

The only required application argument is the exact binary under test:

```sh
FPMNG_BIN=/absolute/path/to/php-fpm-ng \
  tests/frameworks/symfony/run.sh
```

The default `SERVICE_MODE=docker` starts the pinned MySQL 8.4.6 and Redis
7.4.2 services from `compose.yaml`, using private host ports (13306 and 16379,
advanced when occupied). The runner creates a unique MySQL database and Redis
DB/prefix for every run. It never calls `FLUSHALL` or `FLUSHDB`. Its own
Compose project and volume are removed at exit; the report and logs remain in
`.runs/`.

To use the shared test box or another already-running service, provide explicit
endpoints and use a private Redis DB index:

```sh
FPMNG_BIN=/absolute/path/to/php-fpm-ng \
SERVICE_MODE=external \
MYSQL_HOST=192.168.8.50 MYSQL_PORT=3306 \
MYSQL_USER=bench MYSQL_PASSWORD=bench \
MYSQL_ADMIN_SUDO=1 \
REDIS_HOST=192.168.8.50 REDIS_PORT=6379 REDIS_DB=15 \
  tests/frameworks/symfony/run.sh
```

`MYSQL_ADMIN_*` is used only to create and remove the run-specific database;
`MYSQL_ADMIN_SUDO=1` uses passwordless local `sudo mysql` for an administrator
when the application user cannot create databases. The application uses
`MYSQL_USER` and `MYSQL_PASSWORD`. Set `PHP_BIN` and
`COMPOSER_PHAR` when Composer is not on `PATH`. Set
`FPMNG_REDIS_EXTENSION` to a compatible `redis.so` when phpredis is available.
`FPMNG_EXPECTED_SHA256` can make a known binary hash mandatory.

The runner prints and saves the source commit, binary path, binary SHA-256,
`php-fpm-ng -v`, loaded Redis client, and required php-fpm-ng strings before it
claims a result. It rejects a binary missing the current `FPMNG_SHARED_INCLUDES`,
`http.front_controller`, `fiber.revalidate_freq`, or `fiber.isolate_statics`
markers as `NOT MEASURED`; this prevents an old or wrong PHP-FPM binary from
being reported as a framework result.

## Scenarios and outcomes

Positive scenarios make eight concurrent requests and assert response data,
not only HTTP status:

- `/mix`: each request's MySQL row, Doctrine entity, direct Redis value, and
  Redis cache value;
- `/session`: eight distinct session IDs and users, followed by a second round
  with the same cookie jars and `count=2`;
- `/me`: four `alice` and four `bob` requests with Basic credentials, followed
  by a cookie-only round with no `Authorization` header;
- `/identity`: distinct kernel, container, request, EntityManager, and DBAL
  objects while requests are held at a Redis blocking gate.

The negative controls are retained in every run:

- removing `FPMNG_SHARED_INCLUDES` must reproduce the Composer redeclaration on
  request two;
- `fiber.isolate_statics` on a classic pool and the unsupported `async`
  executor must be rejected during configuration validation.

Every row is reported as `PASS`, `ERROR`, or `NOT MEASURED`. An assertion error
is never converted to a skip, and a non-zero exit code is returned if a row is
`ERROR`. The first probe intentionally leaves the following task-matrix items
as `NOT MEASURED`: `APP_ENV=prod`, more than one worker,
`fiber.revalidate_freq`, Twig/forms/validation/messenger, and the long RSS run.
