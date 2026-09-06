# 022 — Process-wide functions still reachable from a request on the fiber executor

**Track:** nice-to-have. This task only applies to `pool.executor = fiber`,
which is moving behind a build flag that is **off by default**, so a stock
binary does not contain this code at all. The `Priority:` line below is the
priority *within* the fiber track — it is not a claim against the HTTP,
cron, scheduler or proxy work, which is where the project is focused.

**Priority:** medium. One known hole, and no systematic check that there are not
more.
**Status:** open.

## Context

The fiber executor runs several requests in one process, so any PHP function
acting on **the process** affects every request in flight, not the caller.

`fpm_coop_container_start()` already refuses a list of `pcntl_*` functions via
`zend_disable_functions()` — the same mechanism FPM itself uses to apply
`php_admin_value[disable_functions]` after fork. The reasoning and the list are
in `sapi/fpmng/fpm/fpm_pool_coop.c` next to `fpm_coop_disabled_functions`.

`posix_kill()` is a known hole: it can signal the worker itself, which affects
every in-flight request, and it is not on the list.

More importantly, the list was assembled by reasoning about `pcntl`, not by
auditing what else can reach the process. Nobody has checked systematically.

## Problem

Audit which PHP functions can affect process-wide state in a way that breaks the
one-process-many-requests model, then decide per function: refuse, make
per-request, or document as accepted.

## Acceptance criteria

1. An enumerated list of candidates with a stated method for how it was
   produced, so a reader can judge its completeness. Starting points:
   - signal delivery: `posix_kill`, `posix_setuid`/`setgid`, `pcntl_*` (already
     partly refused)
   - process attributes: `chdir`, `umask`, `putenv`, `setlocale`
   - resource and limit changes visible outside the request
   - anything that ends the process: `exit`/`die` semantics under a fiber,
     `register_shutdown_function` (already known to be process-wide)
2. For each: refuse / isolate / accept, with the reason.
3. Refusals follow the existing precedent — refused at container start, logged
   once with a message naming why, so the operator sees it in the log rather
   than discovering it from a fatal error.
4. Anything accepted as a known limitation is documented in
   `docs/fiber_errors.md` alongside the existing entries, not left implicit.
5. The `async` executor is covered by the same decisions (see task 019).

## Notes

- `chdir` deserves specific attention: `fpm_coop_execute()` deliberately uses
  `zend_execute_scripts()` rather than `php_execute_script()` **because** the
  latter does a `chdir` to the script's directory, and the working directory is
  per process while requests run concurrently. The comment there explains it.
  That is a case where the model was already protected; a userland `chdir()`
  call is the same hazard from the other direction.
- Refusing a function is a compatibility break for whoever used it. Each refusal
  should be justified by a concrete way it breaks concurrency, not by caution
  alone.
