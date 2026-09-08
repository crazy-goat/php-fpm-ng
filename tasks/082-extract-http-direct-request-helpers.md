# 082 — Extract the shared HTTP-direct request/path helpers

Status: open
Type: refactor
Related: `sapi/fpmng/fpm/fpm_http_direct.c`, `sapi/fpmng/fpm/fpm_http_direct_worker.c`

## Why

Request parsing, front-controller/path-info resolution and directive
validation are duplicated between the classic and the worker executor. The
worker copy was written by reading the classic one, and the divergence is not
hypothetical: the task 073 review found two the duplication had already caused
— the accepted 1xx status range and the dropped-header contract — both fixed
in that branch, in one copy each.

Confirmed as task-worthy twice: by the task 073 branch review and again by the
task 079 review. It has stayed in the gitignored `findings.md` without a task
file since 2026-09-08, which is why this file exists.

## Scope

Extract the shared helpers into one file under `sapi/fpmng/fpm/` used by both
executors — new behaviour in a new file, per `workflow.md`. What is genuinely
per-executor (the response path, the pending table, the worker's queue) stays
where it is; the extraction is about the *request* side.

Bring the 64 KB stack array with it. `fpmng_worker_request_env()` declares
`char name[FPM_WORKER_HEADERS_MAX + 6]` per header-loop iteration, and
`FPM_WORKER_HEADERS_MAX` is 64 KB, so every header touches a 64 KB stack
frame. That was rejected as a task of its own precisely because the constant is
load-bearing for the bounds check beside it: sizing the buffer from the actual
header-name limit is safe to do while the code is being moved, and unsafe to do
in isolation.

## Acceptance criteria

- Both executors call one implementation; neither keeps a private copy of the
  path resolution or the `rejects`/`validate` pair.
- The two known divergences cannot recur by construction, not by being fixed
  again in two places.
- `sapi/fpmng/tests/fpmng-http-direct-*.phpt` and the worker tests stay green,
  and the classic transport's behaviour is unchanged — this is a refactor, so a
  behaviour change is a bug in it.

## Out of scope

- The response/flush paths, which differ for a real reason
  (`evhttp_send_reply()` under a worker-driven base versus the master's).
- Anything about `pool.executor = fiber` or `async`; see task 084.
