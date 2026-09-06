# Known issues in the Async executor

State as of 2026-09-06. `pool.executor = async` is an experiment and is
currently rejected during configuration validation on every engine. It is not
intended for production use. `fpm_pool_async_validate()` logs that the
executor lacks the hardening required for multiple requests in one process
(`fpm_pool_async.c`).

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

## Why parity is not safe yet

### `max_execution_time` was accepted silently

Before the refusal, `fpm_pool_async_validate()` checked only the presence of the
True Async API, ZTS, a registered scheduler/reactor and `pm = static`. It did not
check `max_execution_time`. The file's own header lists `max_execution_time`
among the state SHARED across requests in flight (`fpm_pool_async.c:6-8`), so a
non-zero value would be accepted and unenforced — the Zend timeout is one
`setitimer()`/`SIGPROF` per process, and the process serves N requests.

The Fiber implementation rejects this configuration; Async needs equivalent
validation before it can be enabled.

### Opcache was not checked

Fiber rejects opcache when enabled, because the auto-globals mask and file
timestamps are reset once per request container. Async has the same model and
**did not have this check**. This was a gap in the POC, not a deliberate
allowance.

### The process-wide `pcntl` API is not blocked

`pcntl_signal()` sets one table per process
(`PCNTL_G(php_signal_table)` and `SIGG(handlers)`), and `pcntl_fork()`/`pcntl_exec()`
duplicate or replace the whole multi-request process along with its scheduler,
descriptors and requests in flight. The POC did not remove these functions.

### `set_time_limit()` is not cleaned up after the request

`set_time_limit()`/`ini_set()` goes through `OnUpdateTimeout` to
`zend_set_timeout()` and arms the process timer. Fiber restores the ini entry
after the script; the POC did not, so the value and the timer outlived the
request that changed them.

## Decision from task 019: refusal

Fiber's safeguards were deliberately placed in `fpm_pool_coop.c`, in code that
the fiber type already has — so as not to add an `if (type == ...)` to the
core (the contract from `fpm_pool_type.h`). Async has its own core and a
different coroutine-switching mechanism, so merely calling
`fpm_coop_container_start()` would not provide correct isolation.

The chosen decision is **refusal**: `fpm_pool_async_validate()` rejects
`pool.executor = async` on every engine with a message naming the missing
hardening and the `classic`/`fiber` alternatives. The POC implementation stays
in the tree for future work, but configuration cannot start it.

The concurrent-session, ini-isolation, autoglobal and persistent-connection
measurements were not repeated for Async. After choosing refusal, there is no
supported execution path; the measurements in section 3t of `docs/NOTES.md`
remain historical POC results, not a compatibility claim.

## Beyond pcntl — shared with Fiber

`posix_kill(getmypid(), SIGTERM)` from a script kills the process with N
requests in flight. Applies to both executors and is not blocked in either.
