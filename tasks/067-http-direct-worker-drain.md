# 067 — Per-worker drain trigger for zero-downtime deploys

Status: open
Depends on: —

## Why

HTTP-direct workers already drain gracefully on SIGQUIT: the listener is
removed and in-flight requests complete before exit (tested in task 054). What
is missing for rolling deploys is a per-worker, load-balancer-friendly
trigger: mark one worker as "retire after the current request", let the master
spawn its replacement immediately, and let the old worker exit only when its
last request finishes — blue-green per worker without pausing the pool.

## Scope

A master-side mechanism to retire individual workers on demand (signal or
status/control surface, decided during implementation): the marked worker stops
accepting, finishes in-flight requests, and exits; the master already replaces
crashed/recycled workers, so replacement capacity arrives without pool-wide
reload. Interaction with `pm.max_requests`, graceful reload, and the task 063
connection policy must be defined and tested.

## Acceptance criteria

- Data-asserting lifecycle test: retire worker A while a slow request runs on
  it; A finishes the request, exits; a new worker takes its place; other
  workers serve continuously; no request is dropped or duplicated.
- Retiring all workers one by one equals a graceful rolling restart of the
  pool, verified by a continuous client observing zero failures.
- Marking is idempotent and safe when the worker is already retiring; master
  logs/scoreboard reflect the retiring state (task 066 counters if landed).
- The existing graceful reload/stop tests from task 054 keep passing.

## Out of scope

- Orchestrator/k8s integration or health-check endpoints for load balancers;
  this task provides the mechanism, exposure can follow.
- Changing master supervision for other pool types.
