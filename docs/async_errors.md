# Known issues in the Async executor

State as of 2026-09-06. `pool.executor = async` is an experiment and is not
intended for production use. It only works on the `true-async/php-src` fork
with `ext/async` loaded; on the upstream engine
`fpm_pool_async_validate()` rejects the pool with a readable message
(`fpm_pool_async.c:68`).

## Why this file exists

Async has **exactly the same shape as Fiber** — one process, one
`php_request_startup()` for the whole life of the process (a "request
container"), many requests in flight, each in its own coroutine
(`fpm_pool_async.c:1-9`). This shape produces the same three problems as
described in [fiber_errors.md](fiber_errors.md).

The difference is that **Fiber got safeguards, and Async did not**. Fiber
goes through the coop core (`fpm_pool_coop.c`), which:

- rejects `max_execution_time != 0` in `fpm_coop_validate()`;
- rejects opcache when enabled;
- removes the process-wide `pcntl` functions via `zend_disable_functions()`
  in `fpm_coop_container_start()`;
- restores the ini entry after `set_time_limit()` in `fpm_coop_req_run()`.

Async has **its own** `validate()` and **its own** `child_main()`
(`fpm_pool_type.c:124,146`) and does not call `fpm_coop_container_start()` —
checked: the only call in the whole tree is `fpm_pool_fiber.c:358`.
None of the mechanisms above cover it.

## What is specifically left open

### `max_execution_time` is accepted silently

`fpm_pool_async_validate()` checks only: presence of the True Async API, ZTS,
a registered scheduler/reactor and `pm = static`. It does not check
`max_execution_time`. The file's own header lists `max_execution_time` among
the state SHARED across requests in flight (`fpm_pool_async.c:6-8`), so a
non-zero value is accepted and unenforced — the Zend timeout is one
`setitimer()`/`SIGPROF` per process, and the process serves N requests.

Fix symmetric to Fiber's: reject in `fpm_pool_async_validate()`.

### Opcache is not checked

Fiber rejects opcache when enabled, because the auto-globals mask and file
timestamps are reset once per request container. Async has the same model and
**does not have this check**. This is a gap, not a deliberate allowance.

### The process-wide `pcntl` API is not blocked

`pcntl_signal()` sets one table per process
(`PCNTL_G(php_signal_table)` and `SIGG(handlers)`), and `pcntl_fork()`/`pcntl_exec()`
duplicate or replace the whole multi-request process along with its scheduler,
descriptors and requests in flight. Async does not remove these functions.

### `set_time_limit()` is not cleaned up after the request

`set_time_limit()`/`ini_set()` goes through `OnUpdateTimeout` to
`zend_set_timeout()` and arms the process timer. Fiber restores the ini entry
after the script; Async does not, so the value and the timer outlive the
request that changed them.

## Asymmetry introduced deliberately

Fiber's safeguards were deliberately placed in `fpm_pool_coop.c`, in code that
the fiber type already has — so as not to add an `if (type == ...)` to the
core (the contract from `fpm_pool_type.h`). A side effect is that Async, which
has its own core, inherited none of it.

This is not an oversight, it is a deferred decision: Async requires an engine
fork, so nobody runs it by accident, and aligning the safeguards only makes
sense once it stops being a POC. Options:

1. **Copy** the checks into `fpm_pool_async_validate()` and the block into
   `fpm_pool_async_child_main()` — simplest, duplicates code.
2. **Extract** the shared part (ini validation + the list of blocked
   functions) into a function shared by both cores — cleaner, because the
   problem comes from the shared SHAPE, not from shared implementation.

Recommendation: option 2, but only at the next serious round of work on Async.
As long as `validate()` rejects the pool on every upstream engine, the
practical risk is zero.

## Beyond pcntl — shared with Fiber

`posix_kill(getmypid(), SIGTERM)` from a script kills the process with N
requests in flight. Applies to both executors and is not blocked in either.
