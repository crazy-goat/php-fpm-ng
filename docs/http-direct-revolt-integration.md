# amphp/Revolt on the HTTP-direct scheduler

Status: proposal (design note; input to tasks 070, 072, 073)

Revision 2026-09-08: rewritten around two worker modes and around PHP owning
the event loop. Revision 1 described a three-level ladder (A/B/C) climbed from
inside the existing request callback; the reentrancy measurement below shows
that ladder cannot be climbed in that direction, and the two-mode split makes
its top level considerably cheaper than revision 1 assumed.

## Goal

HTTP-direct (task 054, merged in PR #34) gives each FPM worker its own libevent
event loop serving HTTP and PHP. This note defines how amphp applications — and
anything else built on the Revolt event loop — run on that worker-owned
scheduler, without inventing a new async API for php-fpm-ng.

## The proposition, stated as a product and not as plumbing

FPM already owns everything that is expensive to build and boring to maintain:
a master process, `pm`, the scoreboard, graceful reload, worker recycling on
`pm.max_requests`, TLS with certificate renewal (task 040), access logs,
metrics, privilege dropping and per-pool limits. What it historically could not
offer is an event loop that PHP can reach.

Every other route to async PHP — amphp/http-server, ReactPHP, Swoole,
RoadRunner — hands you the event loop and makes you rebuild that operational
layer yourself as a bespoke daemon. HTTP-direct inverts this: the socket already
terminates inside a supervised worker, so exposing that worker's loop to PHP
turns php-fpm-ng into an application server where the operator surface is FPM's
and the application surface is the standard PHP async ecosystem.

The resulting continuum is the differentiator, and it is one binary and one
configuration file:

| Mode | Configuration | What PHP sees | Who it is for |
|---|---|---|---|
| Classic direct | `pool.type = http-direct` | One script per request, blocking, full per-request isolation, one request in flight per worker | Existing Laravel/Symfony applications; nothing to learn, nothing to rewrite |
| Worker direct | `pool.type = http-direct` + `pool.executor = worker` | Script boots once, owns the loop, handles many concurrent requests as userland fibers | New applications and API gateways written on amphp (or any Revolt-based stack) |

Nobody else offers both from one process manager: Swoole requires a rewrite,
RoadRunner requires a Go binary next to PHP, and FrankenPHP's worker mode
exposes no event loop, so `await` has nothing to stand on.

## Key insight: Revolt's driver seam is the whole integration

Revolt (amphp's event loop) has pluggable drivers: a `StreamSelectDriver`
fallback plus drivers backed by `ext-uv`, `ext-ev` and `ext-event`. The seam
exists and is proven, so php-fpm-ng should plug into it rather than publish an
ad-hoc scheduling API. Then `amphp/*` (http-client, socket, redis, mysql) runs
on the worker loop with zero library changes, and `amphp/http-server` is no
longer needed for the accept/socket layer — the worker *is* the HTTP server —
while its router and middleware remain useful userland code.

The driver contract is small and stable (revolt/event-loop v1.0.9,
`src/EventLoop/Internal/AbstractDriver.php:399-444`): a driver subclasses
`AbstractDriver` and implements `activate(array $callbacks)`,
`dispatch(bool $blocking)`, `deactivate(DriverCallback $callback)` and
`now()`, and overrides `stop()`/`getHandle()`. `Driver\EventDriver` — the
ext-event driver, i.e. libevent 2 — is a 200-line file and is the direct
template for ours.

**Therefore the SAPI does not implement a Revolt driver in C.** It exposes
libevent primitives (create/enable/disable/free an fd, timer or signal event;
pump the loop once, blocking or not) and the driver is ~150 lines of userland
PHP on top. That keeps Revolt out of the C code entirely, keeps the
library-version risk in userland where it can be edited without a rebuild, and
gives task 072 the cheapest possible completeness proof for its event API: a
third-party event loop runs unmodified on it.

## Why PHP must own the loop, with the measurement

Today a direct worker runs PHP *inside* an evhttp callback:
`evhttp_set_gencb(w.http, fpm_direct_handle, &w)`
(`sapi/fpmng/fpm/fpm_http_direct.c:508`) fires under
`event_base_dispatch(w.base)` (`fpm_http_direct.c:518`), and
`php_request_startup()` / `php_execute_script()` run in that callback
(`fpm_http_direct.c:412`, `fpm_http_direct.c:431`).

A Revolt driver mapped onto that same event base cannot work in that direction.
Userland `Amp\async(...)->await()` in `{main}` reaches
`Suspension::suspend()`, which drives the loop — so the driver's `dispatch()`
would call `event_base_loop()` on a base that is already looping. Measured on
the test box (libevent 2.1.12-stable, Ubuntu 24.04, `gcc` probe calling
`event_base_loop(base, EVLOOP_ONCE|EVLOOP_NONBLOCK)` from inside a timer
callback of the same base):

```
libevent 2.1.12-stable
nested event_base_loop returned -1
[warn] event_base_loop: reentrant invocation.  Only one event_base_loop can run on each event_base at once.
```

So it is not "callbacks fire late", as revision 1 of this note assumed — it is
`await` failing outright. The fix is to invert ownership rather than to nest:

- the worker calls `php_request_startup()` **once** and executes a worker
  script, which installs the driver and calls `Revolt\EventLoop::run()`;
- `run()` → `dispatch()` → our `event_base_loop(base, EVLOOP_ONCE)`, so there is
  exactly one loop invocation and no recursion;
- evhttp's callback calls a userland handler, which starts a fiber per request;
  when that fiber suspends, control returns to the resumer — inside the evhttp
  callback — the callback returns, and the loop keeps running. This is precisely
  how amphp already behaves on `ext-event` today.

## The consequence revision 1 missed: no SAPI fiber executor is needed

Revision 1 called concurrent requests per worker "level C" and made it depend on
a direct fiber executor (task 070's verdict). In worker mode that dependency
disappears: concurrency is N userland fibers on one Revolt loop, which
amphp/Revolt already implements and tests. The SAPI schedules I/O, not PHP
execution.

This splits the roadmap into two independent tracks instead of one ladder:

- **classic direct** keeps the open question of whether the *SAPI* should
  suspend requests (`sapi/fpmng/fpm/fpm_pool_fiber.c`, tasks 070 and the
  `nice-to-have` fiber track) — that is where request-transparent concurrency
  for unmodified applications would come from;
- **worker direct** needs none of it, and is reachable now.

## Honest limits of worker mode

These are properties of the mode, not defects to be fixed later, and must be
documented wherever it is offered:

- **No per-request isolation.** The script boots once, so state leaks between
  requests exactly as in Swoole and RoadRunner. php-fpm-ng has one asset here
  that neither has: `fiber.isolate_statics`
  (`sapi/fpmng/fpm/fpm_pool_coop_statics.c`, cost measured in task 052) already
  resets declared static state per request; whether it can serve worker mode is
  a follow-up question, not a POC one.
- **One blocking call stalls the worker.** `PDO::query()`, `curl_exec()`,
  `file_get_contents()` on a socket — any of them freezes every other request on
  that worker. amphp offers async mysql, redis, postgres and http-client, but
  there is no async PDO, so Doctrine and Eloquent do not become concurrent by
  moving into this mode. The mode is for applications written for it.
- **Output is not per-request.** With one `php_request_startup()` per worker
  lifetime there is no per-request output buffer, so `echo` belongs to the
  worker, not to a response; a handler returns its body instead. Mapping
  `echo`/`header()` onto the fiber-current request (as FrankenPHP does by
  swapping superglobals) is a later decision.
- **Scoreboard semantics.** `pool.type = status` reports idle/active/requests
  per worker; a worker serving 50 connections at once does not fit that shape.
  Related open work: tasks 064 and 066.

## Roadmap adjustments

1. **Amend task 072**: design the SAPI event API as libevent primitives
   sufficient for a *userland* Revolt driver, and treat "unmodified
   revolt/event-loop runs on it" as the acceptance proof. The API is not
   Revolt-specific; Revolt is the test.
2. **Amend task 070**: the fiber-executor verdict concerns classic direct only.
   Concurrent requests per worker, SSE fan-out and `fpm_push()` in worker mode
   are delivered by userland fibers; the task should measure the *classic*
   head-of-line cost and stop treating "fibers" as one word covering both modes.
3. **New task 073** (this note's POC): worker mode plus loop primitives plus a
   userland Revolt driver, proven by an amphp hello-world with a concurrent
   one-second sleep.
4. **Tasks 055, 063, 066 unchanged**: worker-level I/O policy and observability,
   independent of the userland scheduler.
5. **Worker mode is an executor, not a type.** `fiber` and `async` are already
   type *variants* selected from `pool.executor` by `fpm_pool_type_resolve()`
   (`fpm_pool_type.c`: `fpm_pool_http_fiber`, `fpm_pool_fastcgi_ng_async`), each
   swapping `child_main` wholesale — which is exactly what worker mode does to
   `http-direct`. Revision 1 of this note implied a second `pool.type`; that
   would have made the operator learn a new type name for an unchanged
   transport, and the "different lifecycle" argument for it does not hold,
   because the fiber executor already gives up per-request isolation of the
   function table (see the comment in `fpmng-fiber-sleep-concurrency.phpt`). So
   task 073 ships `pool.type = http-direct` + `pool.executor = worker`. The
   variant is declared as data on the type — `extra_executor` /
   `extra_executor_type` in `fpm_pool_type_s` — so `resolve()` still never
   compares a type name.

## Current state

Task 073 implements the POC described above. Amendments 1 and 2 are written into
`tasks/070-*.md` and `tasks/072-*.md`; item 5 is not scheduled.
