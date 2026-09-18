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
  suspend requests (the fiber executor, now on branch `async`; tasks 070 and
  the `nice-to-have` fiber track) — that is where request-transparent
  concurrency for unmodified applications would come from;
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
- **Scoreboard semantics.** The status page reports idle/active/requests
  per worker; a worker serving 50 connections at once does not fit that shape.
  Related open work: tasks 064 and 066.

## Buffered streams: read until the read comes up short

**The rule for anyone writing a driver or a handler on these primitives: when a
read watcher fires, read until the read comes up short, not once per event.**
No fixed chunk size is safe.

`fpmng_worker_event_create()` arms a libevent watcher on a *descriptor*, while
userland works on a *PHP stream*. A stream that buffers above the descriptor —
TLS above all — can hold decrypted bytes while the descriptor is genuinely
empty, so no readability event will ever be produced for them. Task 075
measured it over keep-alive TLS with an 8 KiB response and one read per readable
event, changing nothing but the read chunk:

| read chunk | result                                          |
| ---------- | ----------------------------------------------- |
| 512        | stranded, 257 of 8192 body bytes after 1 read   |
| 1024       | stranded, 769 of 8192 body bytes after 1 read   |
| 4096       | stranded, 3841 of 8192 body bytes after 1 read  |
| 8192       | stranded, 7937 of 8192 body bytes after 1 read  |
| 65536      | complete, 8447 bytes in 1 read                  |

Matching PHP's default `chunk_size` of 8192 does not help: the stranded amount
is whatever the peer happened to send. And the failure was silent — no log
line, no error, the request simply never finished.

Task 079 removed the trap rather than documenting it, by copying what PHP's own
`stream_select()` does. `stream_select()` re-casts every stream on every call
(`ext/standard/streamsfuncs.c:673`), and for an SSL stream that cast is not a
passive lookup: while the read buffer is empty it moves `SSL_pending()` bytes
into it (`ext/openssl/xp_ssl.c`, case `PHP_STREAM_AS_FD_FOR_SELECT`). We cast
once at `event_create()` time and kept the descriptor, which is why the bytes
became permanently invisible. `fpmng_worker_loop()` now walks its read watchers
before letting libevent sleep, re-casts each stream, and calls `event_active()`
on any whose buffer is non-empty.

Three consequences an author should know:

- **Read watchers are level-triggered.** A watcher whose stream still holds
  buffered bytes fires again on the next iteration, and the loop will not sleep
  while that is true. The rule above is therefore about CPU, not correctness:
  a handler that reads one small chunk per event still finishes, it just makes
  the loop spin until the buffer drains.
- **A spin is named in the log.** After 100 consecutive iterations with a buffer
  that has not shrunk, the pool logs
  `a read watcher was invoked 100 times in a row with N byte(s) left in the
  stream's userland buffer and nothing consuming them`. 100% CPU with nothing
  in the log would have been a worse trade than the hang this replaced.
- **`fpmng_worker_stream_has_buffered($stream)`** answers the question directly,
  for a driver that would rather decide than be told. It re-casts as a side
  effect, so `false` means the bytes are genuinely not there yet rather than
  merely not fetched. `amphp/byte-stream` does the equivalent by reading
  directly before arming a watcher, which is why it never hit the stranding;
  this makes that trick writable without knowing about TLS.

**Filters are covered by the same mechanism**, and it is worth saying so
because `php_stream_cast()` looks at first glance as though it refuses them.
The refusal is conditional: `main/streams/cast.c:307` reads
`if (php_stream_is_filtered(stream) && castas != PHP_STREAM_AS_FD_FOR_SELECT)`,
and `PHP_STREAM_AS_FD_FOR_SELECT` is exactly the cast both
`fpmng_worker_event_create()` and the loop use. So a filtered socket stream is
accepted by `event_create()`, and because a filter's output lands in the same
`readbuf` the detector inspects, a filtered read watcher gets activated for its
buffered bytes just like a TLS one. Any other cast of a filtered stream — to a
`FILE*` or a plain fd — still fails, which is why the message exists.

## Roadmap adjustments

1. **Amend task 072**: design the SAPI event API as libevent primitives
   sufficient for a *userland* Revolt driver, and treat "unmodified
   revolt/event-loop runs on it" as the acceptance proof. The API is not
   Revolt-specific; Revolt is the test.
2. **Amend task 070**: the fiber-executor verdict concerns classic direct only.
   Concurrent requests per worker, SSE fan-out and `fpm_push()` in worker mode
   are delivered by userland fibers; the task should measure the *classic*
   head-of-line cost and stop treating "fibers" as one word covering both modes.
   (2026-09-18, issue #342: SSE fan-out no longer claims a Revolt driver or
   fibers specifically — `examples/http-direct-worker-sse/` delivers it on the
   plain `fpmng_worker_*` primitives with plain Fibers; the "fan-out" is within
   one worker either way, see issue #182 for the cross-worker question.)
3. **New task 073** (this note's POC): worker mode plus loop primitives plus a
   userland Revolt driver, proven by an amphp hello-world with a concurrent
   one-second sleep.
4. **Tasks 055, 063, 066 unchanged**: worker-level I/O policy and observability,
   independent of the userland scheduler.
5. **Worker mode is an executor, not a type.** `fiber` and `async` were already
   type *variants* selected from `pool.executor` by `fpm_pool_type_resolve()`
   (`fpm_pool_type.c`), each swapping `child_main` wholesale — which is
   exactly what worker mode does to `http-direct`. Revision 1 of this note
   implied a second `pool.type`; that would have made the operator learn a new
   type name for an unchanged transport, and the "different lifecycle"
   argument for it does not hold, because the fiber executor (now on branch
   `async`) already gave up per-request isolation of the function table. So
   task 073 ships `pool.type = http-direct` + `pool.executor = worker`. The
   variant is declared as data on the type — an entry in the `executors` list
   on `fpm_pool_type_s` (issue #76 generalised task 073's original
   `extra_executor` pair into that list) — so `resolve()` still never compares
   a type or executor name.

## Current state

Task 073 implements the POC described above. Amendments 1 and 2 are written into
[#68](https://github.com/crazy-goat/php-fpm-ng/issues/68) and
[#70](https://github.com/crazy-goat/php-fpm-ng/issues/70); item 5 is not
scheduled.

Adjustment 1 above asserts "the API is not Revolt-specific; Revolt is the test".
Task 075 tested that claim with a second, independent consumer and it held: a
`React\EventLoop\LoopInterface` over the same primitives serves concurrent
requests on promises rather than fibers, with no change to `sapi/fpmng/`
(`examples/http-direct-worker-react/README.md`). The one method it cannot
implement is `addSignal()`, which is deliberate — the FPM master owns the
signals. `futureTick()` was the only part needing a primitive no Revolt driver
ever calls for its own sake, `fpmng_worker_loop(false)`, and it was already
there.
