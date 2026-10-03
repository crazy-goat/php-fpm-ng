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
- synchronous queue dispatch and synchronous broadcasting;
- the cache-backed rate limiter (per-request keys, attempts asserted);
- mail attribution through the log transport (asserted on `laravel.log`
  lines, including no interleaved mid-line writes);
- Blade rendering with a per-request view composer;
- implicit route model binding;
- Eloquent model observers and global scopes registered per request.

## Statics audit (the systematic answer)

The four-entry list below was found empirically, one scenario at a time. The
audit replaces that with a measurement: the `/statics-audit` route touches
every state-keeping subsystem, snapshots **every static property of every
declared class**, blocks on a real MySQL suspension, and snapshots again.
Any static whose value changed across the suspension was overwritten by
another concurrent request — that static holds per-request state and belongs
on the isolation list. `bin/run.sh` runs it twice after the scenario suites:

- **clean audit** (`LARAVEL_AUDIT_EXPECT=clean`, pool list = configured, the
  default and the only mode `bin/run.sh` runs): the probe touches every
  state-keeping subsystem before snapshotting, so a static another request
  overwrites during the suspension appears in the steady-state change set
  even though the pool stays healthy. Anything in that set that is neither
  on the configured list nor a documented benign exclusion is reported as
  `AUDIT_RESULT=UNCOVERED` and fails the run. This is how the list is
  verified — and re-verified after every Laravel version bump.
- **leak audit** (`LARAVEL_AUDIT_EXPECT=leak`, pool list empty) is a manual
  forensic mode, not part of the default run: with no isolation the probe
  itself destabilizes the pool (measured: HTTP 500s, 502s, and MySQL client
  RSET_HEADER protocol corruption — traffic aimed at the shared MySQL
  server). The per-scenario negative controls reproduce empty-list damage
  on isolated routes already.

`bin/statics-scan.sh` is the enumeration half: it lists every static
*property* declaration in the pinned `vendor/laravel/framework` tree, so the
search space the audit discriminates against is on record.

## Configuration snippet (Laravel 13.30.1)

The configured pool uses this versioned list for Laravel 13.30.1 (the same
list `bin/run.sh` passes to the pool; see `docs/frameworks.md`, section
"Laravel: the versioned configuration snippet and how it is verified", for
where each entry came from and the warning about incomplete lists):

```ini
fiber.isolate_statics = Illuminate\\Container\\Container::instance,Illuminate\\Support\\Facades\\Facade::app,Illuminate\\Support\\Facades\\Facade::resolvedInstance,Illuminate\\Database\\Eloquent\\Model::resolver,Illuminate\\Database\\Eloquent\\Model::dispatcher,Illuminate\\Database\\Eloquent\\Model::globalScopes
```

`Model::$resolver` was added by the repository-owned Eloquent probe: with only
the three previously documented entries, concurrent Eloquent queries returned
HTTP 500 with `Cannot execute queries while other unbuffered queries are
active`; adding that fourth entry made the Eloquent and mixed DB/Cache/Redis
scenarios pass. `Model::$dispatcher` and `Model::$globalScopes` were added by
the task 025 observers/global-scopes scenario: without them, a model-event
listener registered by request A does not fire for A's own models, and a
global scope registered by A silently filters B's query (HTTP 200,
`item: null`). This is a measured configuration requirement, not Laravel code
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

**Read the warning in `docs/frameworks.md` before deploying this.** The short
version: when this list is incomplete for a code path your application uses,
Laravel does not crash — it returns a correct-looking HTTP 200 with another
request's data and logs nothing. The audit narrows that risk to measured
subsystems; it cannot eliminate it for code the probe never executed.

## Service provisioning: SERVICE_MODE

`SERVICE_MODE=docker` (the default) starts a private MySQL and Redis via
`compose.yaml` — the same pinned images (`mysql:8.4.6`, `redis:7.4.2-alpine`)
as `tests/frameworks/symfony/compose.yaml` — on free host ports, creates a
per-run database, and tears the whole compose project down (`down --volumes
--remove-orphans`) on exit. Nothing needs to be pre-provisioned except Docker
itself.

`SERVICE_MODE=external` is the previous behaviour: it points at MySQL/Redis
that are already running, via the `LARAVEL_DB_*` / `LARAVEL_REDIS_*`
variables below (e.g. the shared test box). It never issues `FLUSHDB` or
`FLUSHALL` and never creates or drops anything outside its own database name.

**The negative controls must not run against shared services (issue #51).**
They are *designed* to corrupt their database: with an empty static list the
concurrent requests of a scenario share state and the probe writes garbage at
whatever MySQL it can reach — on 2026-09-08 that was the shared test-box MySQL
on 3306, which logged `RSET_HEADER packet additional data length is past 3
bytes`. So under `SERVICE_MODE=external` the negative phase is **skipped** and
the run reports `negative_skipped=1` (the `run-all.sh` aggregate then says
`NOT MEASURED`, not `PASS`). Run the negative suite with `SERVICE_MODE=docker`
(the default, which provisions a private MySQL/Redis), or set
`LARAVEL_NEGATIVE_ALLOW_EXTERNAL=1` only when the external services are
themselves private.

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

On a clean machine (Docker Compose provisioning MySQL/Redis, no external
services required):

```sh
cd tests/frameworks/laravel
PHP=/path/to/php \
FPMNG=/path/to/php-fpm-ng \
REDIS_CLIENT=predis \
./bin/run.sh
```

To point at already-running services instead, add
`SERVICE_MODE=external LARAVEL_DB_HOST=... LARAVEL_REDIS_HOST=...`.

To provision without starting a pool, run `PHP=/path/to/php ./bin/provision.sh`.
The PHP CLI needs the extensions required by the locked Laravel dependencies;
the runner also needs cURL for its concurrent HTTP client.

`REDIS_CLIENT=predis` is an explicit alternative that does not load a
phpredis extension. The test-box evidence in Task 025 uses `phpredis`; a run
with the alternative client must be reported as a different measurement.

`RUN_DIR`, `FCGI_PORT`, `HTTP_PORT`, `LARAVEL_DB_*`, `LARAVEL_REDIS_*`, `PHP`,
`FPMNG`, `SERVICE_MODE`, `MYSQL_PORT`, `REDIS_PORT`, and `REDIS_EXTENSION` are
overridable. The runner defaults to the ports and resources above so parallel
framework tracks do not share them; `SERVICE_MODE=docker` additionally picks
free host ports for its private MySQL/Redis containers, and a private
per-run database name, so concurrent runs on one machine do not collide.
