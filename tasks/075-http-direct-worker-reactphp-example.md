# 075 — Example: worker-mode HTTP-direct + ReactPHP, on the same primitives

Status: todo
Type: example + measurement
Depends on: 073 (merged), 074 (merged)
Related: `docs/http-direct-revolt-integration.md` (roadmap item 1)

## Why

Everything that has ever driven the worker's loop primitives is Revolt. The
phpt (`sapi/fpmng/tests/fpmng-http-direct-worker.phpt`), both examples
(`examples/http-direct-worker/`, `examples/http-direct-worker-mysql/`) and the
design note all go through one 130-line driver subclassing Revolt's
`AbstractDriver`. The claim the design note actually makes is broader —
"design the SAPI event API as libevent primitives sufficient for a *userland*
Revolt driver … The API is not Revolt-specific; Revolt is the test"
(`docs/http-direct-revolt-integration.md`, roadmap item 1). The second half of
that sentence is unproven. A single consumer cannot distinguish "these are
libevent primitives" from "these are the four methods Revolt's `AbstractDriver`
happens to need".

ReactPHP is the independent consumer, and it is independent in ways that reach
the SAPI surface rather than only the PHP above it:

- `React\EventLoop\LoopInterface` has twelve methods against Revolt's four, and
  one of them — `futureTick()` — has no Revolt analogue: a queue that must be
  drained *without* blocking on I/O, which means the driver decides when
  `fpmng_worker_loop(true)` is allowed to block at all.
- fd listeners are persistent, level-triggered and keyed by stream, not
  one-shot callbacks re-armed by the driver. That happens to match
  `FPMNG_WORKER_READ`/`WRITE`, which are `EV_READ|EV_PERSIST` and
  `EV_WRITE|EV_PERSIST` (`sapi/fpmng/fpm/fpm_http_direct_worker.c:806-807`),
  while the timer is one-shot on purpose (`:808-810`) — plausible, untested.
- The application is written in promises, not fibers. If the primitives only
  work under a fiber-based scheduler, that is a limit nobody has looked for.

### The sharp part: ReactPHP is the client 074 said would hang

Task 074 measured its TLS route as working, and was explicit that the reason
lay in the client and not in us: amphp reads speculatively before arming a
watcher, and 074's Outcome records that "a client that waits for readability
first and reads once per event would still hang". ReactPHP is exactly that
client. `React\Stream\DuplexResourceStream::handleData()` runs only from the
loop's read listener and does a single `stream_get_contents($stream, 65536)`
(`vendor/react/stream/src/DuplexResourceStream.php:185-198`, chunk size set at
`:90`) — no speculative read anywhere.

So ReactPHP over TLS is the direct test of the warning written into
`fpmng_worker_event_create()`: the watcher is armed on the raw descriptor from
`php_stream_cast()`, so "a stream with buffered userland data (filters, TLS)
can hold bytes the descriptor will never report as readable"
(`sapi/fpmng/fpm/fpm_http_direct_worker.c:823-827`). For that to be a real
test and not a nominal one, the TLS body has to be well above the 64 KiB read
chunk and has to arrive as many records over time, so that a stranded buffer is
reachable at all.

The TLS transport cannot be MySQL as in 074: `react/mysql` never upgrades the
connection. `CLIENT_SSL` is defined and unused
(`vendor/react/mysql/src/Io/Constants.php:57`), and the connector's `tls`
option applies to `tls://` URIs only. The probe is therefore an HTTPS request
through `react/http` against a TLS origin in the same Compose project.

## Scope

A third example under `examples/http-direct-worker-react/`:

- `FpmngLoop.php` — `React\EventLoop\LoopInterface` implemented over the SAPI
  primitives, installed with `React\EventLoop\Loop::set()`. New code, not a
  port of `FpmngDriver`: the two libraries share no interface.
- `FpmngReactServer.php` — the request-queue bridge, promise-based.
- `app.php` with three routes: `/` (hello world, pid), `/mysql`
  (`react/mysql`, `SELECT SLEEP(1)`), `/tls` (`react/http` GET of a large
  rate-limited body over HTTPS), plus a route that exercises `futureTick()`.
- `compose.yaml` — a pinned MySQL as in 074, plus a pinned TLS origin serving
  a body above the read chunk at a rate that makes the request take about a
  second. Peer verification off, as in 074, and for the same reason.
- 074's `Dockerfile` reused by passing `APP_DIR`, which is what it was written
  for (`examples/http-direct-worker-mysql/Dockerfile:6-10`).
- `build/test-http-direct-worker-react.sh`, with 074's discipline: own Compose
  project name, own port, `down --volumes` in the exit trap, `strings` gate on
  the binary, SKIP without Docker.
- One line in `docs/http-direct-revolt-integration.md` under "Current state",
  recording whether roadmap item 1's claim held.

## Acceptance criteria

- Unmodified, lock-pinned `react/event-loop`, `react/mysql`, `react/socket`
  and `react/http` run on the worker loop: no library patch, and the only new
  PHP is the loop plus the server bridge. Proven by N ≥ 8 concurrent `/mysql`
  requests completing in about one second in total on a single worker pid with
  `pm.max_children = 1`, asserted from the worker's own clock as
  `max(t0) < min(t1)`.
- `futureTick()` is **asserted, not merely implemented**: a route queues K
  ticks and reports the time to drain them, and the harness fails if draining
  K ticks took anything like a blocking loop iteration. A driver that treats
  a tick as a zero-timeout timer would pass the concurrency criterion above
  and fail this one.
- The TLS route is measured and the result is recorded whichever way it goes,
  together with the body size and the number of reads that make the trap
  reachable. If it stalls: the symptom is written down (bytes received against
  `Content-Length`, which watcher was armed and what the loop did next), the
  route is documented as broken and a follow-up task is filed. If it works:
  the `file:line` in ReactPHP that makes it work is named, and the warning in
  `fpmng_worker_event_create()` is amended or confirmed accordingly. A stall
  that is merely suspected is not an acceptable outcome.
- `docker compose up` in the example directory serves all routes, with no PHP
  on the host and no `vendor/` tree prepared by hand.
- The reuse promise 074 made is checked: either the Dockerfile serves this
  application by changing `APP_DIR` alone, or the reason it cannot is recorded
  and the smallest fix that restores the promise is applied.
- The harness SKIPs (exit 0) without Docker or the Compose plugin, uses its own
  project name and its own port — distinct from 28078 and 28088 — and never
  touches a shared MySQL.
- The README states, per route, which primitive it exercises, and maps each
  `LoopInterface` method onto the builtin behind it. "ReactPHP works" is not an
  outcome.

## Out of scope

- Any change to `sapi/fpmng/`. A missing or broken primitive is a finding and a
  new task, not a fix bundled here — including whatever the TLS route finds.
- `addSignal()`/`removeSignal()`: the FPM master owns the worker's signals, so
  the loop throws, exactly as `FpmngDriver::onSignal()` does.
- `react/async`'s `await()` and fibers. This example is promises on purpose:
  fibers are what 073 and 074 already proved. Whether `await()` can drive a
  loop that a SAPI builtin owns is a separate question — record it, do not
  answer it here.
- An amphp-versus-ReactPHP benchmark. Both examples measure the same claim
  about the same primitives; neither is a verdict on a library.
- Wiring the harness into CI: still the open Docker question from task 073.
- Per-request isolation, output buffering, pool sizing.
