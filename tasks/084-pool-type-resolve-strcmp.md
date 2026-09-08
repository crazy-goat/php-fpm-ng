# 084 — `fpm_pool_type_resolve()` still name-compares for `fiber` and `async`

Status: open
Type: refactor
Related: `sapi/fpmng/fpm/fpm_pool_type.c`

## Why

`workflow.md` states the architecture contract plainly: per-pool-type
behaviour belongs in `fpm_pool_type_s` fields and callbacks, never in a
`strcmp(type->name, ...)`. Task 073 added `extra_executor` /
`extra_executor_type` as data, so `worker` needs no comparison — but
`fiber` and `async` predate that and are still resolved by comparing the
executor name inside `resolve()`. The contract is therefore violated in the one
function whose whole job is to honour it, and every new executor pays the cost
of noticing.

Confirmed as task-worthy twice: by the task 073 branch review and again by the
task 079 review, and left in the gitignored `findings.md` without a task file
since 2026-09-08.

## Scope

Generalise `extra_executor` into an executor *list* on `fpm_pool_type_s` and
move `fiber` and `async` onto it, so `resolve()` looks up a name in data
instead of branching on it. Both are behind their own configure flags
(`--enable-fpmng-fiber`, `--enable-fpmng-async`), so the list has to tolerate
executors that are not compiled in — that is the detail that decides the shape.

## Acceptance criteria

- No `strcmp` on an executor or pool-type name survives in
  `fpm_pool_type_resolve()`.
- A build with neither optional executor, one with each, and one with both all
  resolve correctly, and asking for an executor that was not compiled in still
  fails with the message it fails with today.
- The existing pool-type and executor `.phpt` tests stay green.

## Out of scope

- Changing what `fiber` or `async` *do*.
- The HTTP-direct duplication; see task 082.
