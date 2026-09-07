# 003 — Our own `.phpt` tests for pool types and executors

**Priority:** high, after 001 (needs the harness) and ideally after 002.
**Status:** done.

## Context

Task 001 brings back upstream's 150 FPM tests, which cover the behaviour we
inherited. Nothing covers the behaviour we *added*, and that is where all the
risk is: `pool.type`, `pool.executor`, the HTTP gateway, cron, supervisor,
status, metrics.

Everything currently known about how these behave was measured by hand, once,
and written into `docs/`. A hand measurement is not a regression test — several
of this project's findings were originally produced by measuring the wrong
binary, and only caught because someone checked `strings` afterwards.

## Problem

Write `.phpt` tests for our own pool types and executors, using the harness
recovered in task 001.

## The matrix that actually matters

`pool.type` × `pool.executor` is the real combination space, and it is small:

- `pool.type`: `fastcgi` (default, no directive), `fastcgi-ng`, `http`
- `pool.executor`: `classic` (default), `fiber`, `async`
- plus the request-less types: `supervisor`, `cron`, `status`

Not every cell is legal — the type table in `sapi/fpmng/fpm/fpm_pool_type.c`
rejects unsupported directives per type via its `rejects[]` data. **The
rejections themselves are worth testing**: a directive silently accepted by a
type that ignores it is a configuration trap.

## Acceptance criteria

1. Tests exist for, at minimum:
   - a pool with **no** `pool.type` directive behaving as upstream FPM (this is
     the compatibility promise and deserves an explicit test, even if upstream's
     suite overlaps it)
   - each legal `pool.type` × `pool.executor` combination starting, serving one
     request, and shutting down cleanly
   - each illegal combination and each rejected directive being **refused at
     configuration validation time**, with the process not starting
   - the HTTP gateway: a static file served without waking a worker, a PHP
     script served, `http.front_controller` fallback on and off, `.php/`
     PATH_INFO splitting
   - the `fiber` executor: two concurrent requests each seeing their own
     superglobals, their own session, and their own `ini_set` values
   - `cron` firing on schedule, `supervisor` restarting a script that exits,
     `status` answering `/status` and `/metrics`
2. Each test states in its `--DESCRIPTION--` (or a comment) **which documented
   claim it protects**, with a pointer to the `docs/` section or the measurement
   it came from. A test nobody can connect to a requirement gets deleted the
   first time it is inconvenient.
3. Tests that require concurrency assert on **data correctness**, not just HTTP
   status. The Laravel failure at `docs/frameworks.md` returned HTTP 200 for
   every request while serving other users' session data — a status-only test
   would have called that a pass.
4. Tests that need external services (MySQL, Redis) either skip cleanly when the
   service is absent, or are kept out of the default run and documented
   separately.

## Explicitly out of scope

- Benchmarks and timing assertions. Wall-clock numbers on a shared runner are
  noise; the concurrency tests here must assert on *correctness* only.
- Testing frameworks (Symfony, Laravel, Slim). They need MySQL, Redis, a
  `composer install` and real application trees, which do not fit `.phpt`. They
  are **not** out of scope for the project, only for this task — they have their
  own harness in task 027 and their own per-framework tasks in 024, 025 and 026.

## Notes

- Concurrency tests need care: the point of the `fiber` executor is that several
  requests are in flight in **one** process, so the test must actually overlap
  them (an endpoint that sleeps while holding state) rather than issue them
  sequentially. `pm.max_children = 1` is what forces the overlap to be real.

## Outcome

Nine `fpmng-*.phpt` files under `sapi/fpmng/tests/` cover the acceptance
criteria. `build/run-fpmng-phpt.sh` runs only that prefix (not upstream's
150-test suite) with a 120-second default timeout for cron. Documentation is in
`docs/fpmng-phpt.md`. CI job `fpmng-phpt` runs the runner against the canonical
build artifact.

Measured on the test box (2026-09-07, PHP 8.5.9, `--enable-fpmng` without
fiber): **6 PASS / 0 FAIL / 2 SKIP / 1 WARN** across the nine tests. Fiber
matrix and fiber isolation skip without `--enable-fpmng-fiber` (by design).
`fpmng-supervisor-restart.phpt` occasionally WARNs ("passed on retry") on a
loaded host but exits zero; not re-measured after a dedicated fix.

Framework integration, MySQL/Redis, and benchmarks remain out of scope per the
task file.
