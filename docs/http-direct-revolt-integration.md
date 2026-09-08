# amphp/Revolt on the HTTP-direct scheduler

Status: proposal (design note; input to tasks 059, 070, 072)

## Goal

HTTP-direct (task 054, merged in PR #34) gives each FPM worker its own
libevent event loop serving HTTP and PHP. This note defines how amphp
applications — and anything else built on the Revolt event loop — run on that
worker-owned scheduler instead of a separate userland loop, without inventing
a new async API for php-fpm-ng.

## Key insight

Revolt (amphp's event loop) already has pluggable drivers: a
`StreamSelectDriver` fallback plus drivers backed by `ext-uv`, `ext-ev`, and
`ext-event`. The integration seam exists and is proven. Our `ext/fpm`
proposed in task 072 should register a Revolt driver whose backend is the
worker's libevent event base, rather than exposing only an ad-hoc scheduling
API. Then `amphp/*` libraries (http-client, socket, redis, mysql) run on the
worker loop with zero library changes, and `amphp/http-server` becomes
unnecessary: the direct worker *is* the HTTP server, while amphp remains the
concurrency layer.

## Integration levels (increasing)

| Level | What it delivers | Requirements | FPM fibers needed? |
|---|---|---|---|
| A. Driver within a single request | A script uses `Amp\async()`; async I/O inside that request rides the worker's loop; the worker is still blocked for the request duration | Task 072 SAPI event API + a Revolt driver | No — Revolt carries its own fibers in userland |
| B. Driver + cross-request persistence | Watchers survive request end: timers, post-`fpm_respond()` work (task 059), subscriptions | Level A plus watcher scoping: per-request watchers are cancelled at RSHUTDOWN, persistent ones live in a worker-scoped registry | No — still the classic executor |
| C. Revolt as the executor's scheduler | Multiple concurrent requests per worker: a request fiber suspends on I/O and the loop wakes another runnable request fiber | A direct fiber executor (verdict from task 070) with the driver as the single wakeup source | Yes |

Level C makes amphp the natural concurrency API for php-fpm-ng: adopt the
ecosystem instead of inventing our own async surface. It is intentionally a
consequence of the spikes, not a starting point.

## Conflicts to resolve honestly

- **Loop ownership.** Revolt is a singleton per process; a direct worker is
  one process, so the mapping is clean, but the driver must be correct per
  worker and must never touch another worker's loop directly.
- **RSHUTDOWN with pending watchers.** Without hard scoping (cancel-all at
  request end) timers and file descriptors leak across requests. The scoping
  belongs in the SAPI event API, not in userland conventions.
- **Signals.** The FPM master owns SIGQUIT/SIGUSR2 and related lifecycle
  signals; the driver's signal watchers must not take over master-managed
  signals.
- **Classic executor blocks the loop.** amphp callbacks fire only when PHP
  yields execution. Acceptable for levels A/B, but the cost must be measured
  and documented rather than promised away (the task 070 topic).
- **PHP streams versus raw fds.** amphp operates on PHP stream resources; the
  driver must extract file descriptors for libevent registration. This is the
  main technical glue and the first thing to prototype.

## Roadmap adjustments

1. **Amend task 072**: design the SAPI event API against the Revolt driver
   interface (`defer`/`delay`/`repeat`/`onReadable`/`onWritable`/
   `onSignal`/`cancel`/`run`/`stop`) from the start instead of an ad-hoc API.
   The driver doubles as proof that the API is complete.
2. **New task after 072**: "Revolt driver for ext/fpm" covering levels A and
   B, verified with a real amphp library (for example `amphp/http-client`
   inside direct-pool requests), not only synthetic tests.
3. **Amend task 070**: formulate the fiber verdict as "level C requires the
   fiber executor; levels A and B do not", separating what is currently
   lumped under a single word "fibers".
4. **Tasks 055, 063, 066 unchanged**: they concern worker-level I/O policy
   and observability, not the userland scheduler.
5. **Later**: a long-term task for level C ("fiber executor x Revolt") only
   after spikes 070 and 072 return positive measurements.

## Current state

The roadmap amendments and the new driver task are not yet written into
`tasks/`; this note is the agreed plan until then.
