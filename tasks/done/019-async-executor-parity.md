# 019 — `pool.executor = async` has the fiber executor's holes and none of its guards

**Priority:** medium. It is shipped and it is less safe than the executor it
resembles.
**Status:** done. Decision: refusal; recorded in `docs/async_errors.md`.

## Context

The `async` executor (True Async) shares the request-container model with
`fiber`: one `php_request_startup()` per process, several requests in flight,
per-request state swapped around them.

It does **not** share the protections. Only `sapi/fpmng/fpm/fpm_pool_fiber.c:358`
calls `fpm_coop_container_start()`, which is where the fiber executor installs:

- the refusal of `pcntl_*` functions that are process-wide
- the `max_execution_time` validation and per-request ini restoration
- the stream transport interception
- the persistent-connection refusal
- since 2026-09-06: per-request `ext/session` isolation and per-request ini
  value isolation

So a user selecting `async` gets the same sharp edges the fiber executor was
hardened against, with none of the hardening, and nothing tells them.

## Problem

Bring `async` to parity, or refuse it.

## Acceptance criteria

1. An explicit decision between:
   - **parity** — `async` goes through the same container setup and gets the same
     guards and per-request isolation
   - **refusal** — `async` is rejected at configuration validation with a message
     saying it is not hardened, until someone does the work
   - **experimental** — it starts but logs a prominent warning naming what is not
     protected
   `README.md` already calls both `fiber` and `async` experimental and
   not production-ready; that is a start but it is not the same as the executor
   telling the operator at startup.
2. Whichever is chosen is reflected in code, not only in documentation.
3. If parity is chosen, each guard is verified against `async` separately. Do not
   assume that what holds for fibers holds for True Async coroutines — the
   scheduler and the suspension points differ, and `docs/NOTES.md` contains at
   least one earlier claim about fibers that had to be retracted as too strong.
4. The measurements that exist for `fiber` (concurrent sessions, ini isolation,
   autoglobals, persistent connections) are repeated for `async` and recorded, or
   their absence is stated.

## Notes

- The class-statics problem (task 008) and the include model (task 007) apply to
  `async` identically. Whatever is decided there should be checked against this
  executor too rather than rediscovered.

## Outcome

Chose **refusal** rather than parity. `fpm_pool_async_validate()` now rejects
`pool.executor = async` on every engine with a message explaining that the
executor is not hardened for concurrent requests and naming `classic` and
`fiber` as alternatives. The POC implementation remains in the tree for a
future hardening pass, but configuration cannot start it.

Updated the user-facing README files and `docs/async_errors.md` to record the
decision and the known hazards. No new Async measurements were run: the
executor is not a supported execution path; the measurements in
`docs/NOTES.md` section 3t are historical POC evidence.
