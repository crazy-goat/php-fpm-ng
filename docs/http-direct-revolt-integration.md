# amphp/Revolt on the HTTP-direct scheduler

Status: design note updated by issue #190 (worker event API shipped; classic
event-loop re-entry is impossible; the spike verdict is recorded below)

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

The classic direct executor runs PHP *inside* an evhttp callback:
`evhttp_set_gencb(w.http, fpm_direct_handle, &w)`
(`sapi/fpmng/fpm/fpm_http_direct.c:2456`) fires under
`event_base_dispatch(w.base)` (`fpm_http_direct.c:2551`), and
`php_request_startup()` / `php_execute_script()` run in that callback
(`fpm_http_direct.c:2321`, `:2348`). The worker executor reverses ownership and
runs the loop from its PHP worker script.

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
  script (`sapi/fpmng/fpm/fpm_http_direct_worker.c:3640`, `:3705-3712`), which
  installs the driver and calls `Revolt\EventLoop::run()`;
- `run()` → `dispatch()` → our `event_base_loop(base, EVLOOP_ONCE)`, implemented
  in `fpm_http_direct_worker.c:2983-3039`, so there is exactly one loop
  invocation and no recursion;
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

Issue #190 closes the event-API verdict and the shared-scheduler spike #70.
The worker event API shipped in task 073; the classic executor cannot pump its
already-running event base from PHP; and no `ext/fpm` or shared scheduler is
needed for the shipped worker-mode capability. The detailed verdict and the
acceptance run are recorded below. Cross-worker wakeup remains a separate,
unbuilt decision tracked by [#191](https://github.com/crazy-goat/php-fpm-ng/issues/191).

Task 075 tested the claim that "the API is not Revolt-specific; Revolt is the
test" with a second, independent consumer and it held: a
`React\EventLoop\LoopInterface` over the same primitives serves concurrent
requests on promises rather than fibers, with no change to `sapi/fpmng/`
(`examples/http-direct-worker-react/README.md`). The one method it cannot
implement is `addSignal()`, which is deliberate — the FPM master owns the
signals. `futureTick()` was the only part needing a primitive no Revolt driver
ever calls for its own sake, `fpmng_worker_loop(false)`, and it was already
there.

## Issue #190 verdict

### Acceptance run

`build/test-http-direct-amphp.sh` was run on the poligon on 2026-09-23 from the
issue #190 worktree. The worker binary was rebuilt from this checkout against a
pristine `php-src`, so every piece of the identity below belongs to one build:

| | |
|---|---|
| repo HEAD | `92d087878d485af55944b5ea156540bb0d69b11d` (`origin/main` when #190 was started) |
| php-src ref | `php-8.5.9`, commit `dd6e76cce27aaa0ed9f7520648ed1081dfb6af36` — the CI pin at `.github/workflows/build-matrix.yml:182` |
| configure | `--disable-all --enable-fpmng --enable-fpmng-tls --enable-fpmng-acme --enable-session --with-openssl --enable-fpmng-debug-clock` — the canonical `FPMNG_CONFIGURE_FLAGS` at `.github/workflows/build-matrix.yml:220` |
| binary sha256 | `39e7566ce7322998fb11d664d0eb8b37f6ca967f52f681eeff433a98fa1c1eb1` |
| binary marker | `php-fpm-ng/http-direct-worker` present (`strings`, the script's own identity check) |
| `amphp/amp` | `v3.1.3`, from an existing local `vendor/` tree |
| `revolt/event-loop` | `v1.0.9`, from the same tree |
| box / port | `192.168.8.50`, own scratch dir, port 28971 |

The harness was given the vendor tree through `FPMNG_AMPHP_VENDOR` rather than
running Composer, because the CI-configure CLI cannot run it: `--disable-all`
leaves out `ext/phar`. The packages were not modified. A SKIP is not a pass, and
this run did not SKIP.

Output (tabs shown as `\t`):

```
"name": "amphp/amp"\t"version": "v3.1.3"
"name": "revolt/event-loop"\t"version": "v1.0.9"
hello-world: ok (hello world from pid 77840)
concurrent-delay: ok (8 requests in 2s on one worker)
single-worker: ok
test-http-direct-amphp.sh: PASS
```

Thus, the acceptance behavior was observed: eight concurrent requests using
`Amp\delay(1.0)` completed in 2 seconds on one worker (the harness bound is 3 s,
which N serialized one-second sleeps cannot reach), and the harness reported one
worker PID for the whole batch. The 2 s rather than 1 s is curl startup plus
scheduling on a shared box; the concurrency claim is what the bound tests.

An earlier run recorded in the issue used a pre-existing binary with SHA-256
`0be176ba7ab676e8381c1914aad52577e74439a9c8e917f73ee9c3945ad491be` and the
same `amphp`/`revolt` versions. That binary's exact php-src ref and configure
were not re-derived; the run above supersedes it with a fully identified build.

### Executor verdict

- **`pool.executor = worker`: shipped.** The worker's builtin functions are
  registered in `fpm_worker_functions[]`
  (`sapi/fpmng/fpm/fpm_http_direct_worker.c:3280-3302`), including watcher
  creation (`:2795`), loop pumping (`:2986`), deferred response (`:2456`),
  notification stream (`:2140`), and buffered-stream detection (`:2196`).
  They provide watcher create/enable/disable/free, the blocking/non-blocking
  loop pump, loop break, deferred response, notification stream, and buffered
  stream detection. `build/prepare.sh:67-76` documents that fiber and async
  sources have been removed from main, and its source filtering at `:91-94`
  excludes only TLS and ACME prefixes; this worker file remains in the default
  source list. The fiber configure flag is reserved and errors because the
  executor moved to branch `async` (`sapi/fpmng/config.m4:681-690`).

  Two independent userland consumers validate the primitives without SAPI
  changes: the Revolt driver (`examples/http-direct-worker/FpmngDriver.php`) and
  the React loop (`examples/http-direct-worker-react/FpmngLoop.php`). React's
  missing `addSignal()` is deliberate because the FPM master owns signals.

- **`pool.executor = classic`: no in-request event-loop API.** PHP runs from
  `fpm_direct_handle()`, installed as the evhttp generic callback at
  `sapi/fpmng/fpm/fpm_http_direct.c:2456` (the handler itself is `:2179`), and
  `php_request_startup()` / `php_execute_script()` run inside it (`:2321`,
  `:2348`) while the same base is already being dispatched (`:2551`). The
  recorded libevent 2.1.12-stable measurement in "Why PHP must own the loop"
  returns -1 and warns about a reentrant invocation when `event_base_loop()` is
  called from a callback on that base. The only classic-compatible variation
  would let watchers progress between requests, never during PHP execution; it
  serves none of #70's four named consumers and is not proposed. The classic
  base carries a 10 ms `EV_PERSIST` timer (`fpm_http_direct.c:2412`, created
  `:2549`, armed `:2550`) that drives the connection sweep, the accept gate and
  retirement; `fpm_direct_flush()` is intentionally inert to avoid re-entering
  libevent (`:704-720`).

### `ext/fpm` and the shared-scheduler verdict

A bundled extension is compatible with the single-binary design: `ext/fpmng_metrics`
is forced on whenever `--enable-fpmng` is enabled (`ext/fpmng_metrics/config.m4`,
the `PHP_FPMNG` gate), is copied by `build/prepare.sh`, and is included in the
static musl artifact, where the extension set is asserted by
`build/static-full.sh:114-160` (`assert_symbol`). It is not the right
container for worker event functions, however. Pool-type-specific capabilities
are data on `fpm_pool_type_s` (for example, `publishes_acme_challenges`,
`sapi/fpmng/fpm/fpm_pool_type.h:228-235`), while
an extension registered at MINIT is process-wide and pre-fork. The worker
builtins instead register in the forked child against a private temporary
module anchor; the source records the opcache crash and the rejected persistent
module alternative (`sapi/fpmng/fpm/fpm_http_direct_worker.c:3305-3353`), with
the same pattern for ACME builtins (`sapi/fpmng/fpm/fpm_acme_challenge.c:381-407`).
**Recommendation:** keep these as SAPI-registered builtins. Their lifecycle is
worker-child-specific, and the event API is reached through the worker executor;
per-pool-type capabilities belong in `fpm_pool_type_s` data. There is no
capability an `ext/fpm` would add.

"Shared scheduler" refers to separate mechanisms, not one missing subsystem:

1. **Cron scheduling — shipped, master-side:**
   `fpm_cron_schedule.c` and `fpm_pool_cron.c`; cron pools are static with one
   child by construction (`sapi/fpmng/fpm/fpm_pool_cron.c:314-321`), with
   child-side next-due sleep documented in `fpm_pool_cron.h:30-36`; operator
   status state is allocated in shared memory at `fpm_pool_cron.c:329`.
2. **Supervisor restarts — shipped:** shared state in
   `sapi/fpmng/fpm/fpm_pool_supervisor.c:856`; restart backoff and watchdog
   behavior are separate from the worker loop. Follow-up #122 is unrelated.
3. **Event loops — separate and process-local:** the master uses upstream FPM's
   own event API (`fpm_event_set_timer` / `fpm_event_add` in
   `sapi/fpmng/fpm/fpm_process_ctl.c:62-67`), driven from
   `sapi/fpmng/fpm/fpm.c:154`; a classic child owns a private base created at
   `fpm_http_direct.c:2432`; each worker child owns its own exposed base.
4. **Cross-worker wakeup — not built:** tracked as the separate decision issue
   [#191](https://github.com/crazy-goat/php-fpm-ng/issues/191); it is not part
   of this event-API verdict.

### Corrections carried forward

- The claim that `sapi/fpmng` has no locking primitive is false: the ACME
  challenge state takes a bounded-retry spinlock
  (`sapi/fpmng/fpm/fpm_acme_challenge.c:154-157`) and publishes under a CAS
  generation counter (`:147`; the `generation` and `writer_lock` fields are in
  the shared struct at `fpm_acme_challenge.c:30`, `:37`, with the rationale in
  `fpm_acme_challenge.h:24-37`); the gateway also performs lock-free CAS on
  shared counters (`sapi/fpmng/fpm/fpm_http.c:270-321`). These use inherited
  upstream FPM atomics.
- `fpm_shm_alloc()` is used for cron state (`fpm_pool_cron.c:329`), supervisor
  state (`fpm_pool_supervisor.c:856`), gateway counters (`fpm_http.c:3816`),
  metrics (`fpm_metrics.c:39`), ACME challenge state
  (`fpm_acme_challenge.c:58`), TLS reload (`fpm_tls_reload.c:411`), worker
  metrics (`fpm_http_direct_worker_metrics.c:71`), and HTTP-direct operator
  state (`fpm_http_direct_ops.c:124`).
- The earlier audit found three of #70's four layer-2 consumers no longer
  needed its proposed primitive: #61 had a mechanism at `fpm_http.c:270-321`,
  #67 was re-scoped to TTL-only, and #68 gated cross-worker fan-out behind the
  userland demonstrator in #182. The remaining #53 carried its decision rule
  as a comment dated 2026-09-11. This is the state recorded by that audit; the
  issue numbers and scope should be checked before using it as a current plan.
