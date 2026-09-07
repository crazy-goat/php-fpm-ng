# Framework integration test harness

Automated harness for the framework-integration measurements recorded in
`docs/frameworks.md` (Symfony, Laravel, Slim 4), built for task 027. It
replaces the one-off hand measurements that produced those numbers with a
reproducible, repository-owned run.

## The one documented command

```sh
FPMNG_BIN=/path/to/php-fpm-ng [PHP_BIN=/path/to/php] tests/frameworks/run-all.sh
```

`FPMNG_BIN` is required and must point at a built `php-fpm-ng` binary.
`PHP_BIN` (or `PHP`) is optional and defaults to `php` on `PATH`. This single
command provisions everything — Docker Compose starts private, pinned
MySQL/Redis for each framework — and runs all three probes in sequence
against that one binary, from a clean machine with nothing pre-provisioned
except Docker, Composer/PHP, and the binary itself.

It prints one combined summary at the end, one line per framework, reusing
the `PASS` / `ERROR` / `NOT MEASURED` vocabulary each runner already prints
(see below) rather than inventing new status words, and exits non-zero
unless every framework fully passed. A framework that could not run (`NOT
MEASURED`) never counts as a pass, exactly like each individual runner.

Each framework's own runner remains independently invocable and documented;
`run-all.sh` is a thin sequencing layer over them, not a replacement:

- [`symfony/README.md`](symfony/README.md) — `symfony/run.sh`
- [`laravel/README.md`](laravel/README.md) — `laravel/bin/run.sh`
- [`slim4/README.md`](slim4/README.md) — `slim4/bin/run.sh`

## Scope decisions

- **Executor matrix: fiber-only.** All three probes exercise
  `pool.executor = fiber` only. Default/classic-executor coverage is out of
  scope for this harness — see the Outcome section of
  `tasks/done/027-framework-integration-test-harness.md`.
- **Docker Compose by default.** Each framework directory owns a
  `compose.yaml` pinning the same MySQL/Redis versions (`mysql:8.4.6`,
  `redis:7.4.2-alpine`) so a framework-to-framework comparison is never
  confounded by a service-version difference. `SERVICE_MODE=docker` (the
  default for every runner) starts these on private free host ports, creates
  a private per-run database, and tears the whole compose project down
  (`down --volumes --remove-orphans`) on exit — it never issues `FLUSHALL`
  or `FLUSHDB`. `SERVICE_MODE=external` is available on every runner for
  pointing at already-running services (e.g. the shared test box).
- **Sequential, not parallel.** `run-all.sh` runs Symfony, then Laravel,
  then Slim 4, one after another. Each runner defaults to its own fixed
  FastCGI/HTTP ports and, in Docker mode, its own default MySQL/Redis host
  ports; running two at once on a busy machine could make them race for the
  "next free port" and collide with each other. Parallelizing them is future
  work, not part of task 027.

## What "could not run" means here

`NOT MEASURED` covers a missing service, a failed `composer install`, a
`php-fpm-ng` binary missing a required feature marker, or any other
precondition the runner could not satisfy — never a scenario that ran and
whose assertion failed (that is always `ERROR`). See each framework's README
for its own probe coverage and negative controls.
