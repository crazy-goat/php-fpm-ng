# 073 — POC: worker-mode HTTP-direct with a userland Revolt driver

Status: done
Type: POC (experimental, explicitly not a supported feature)
Depends on: —
Related: 070, 072 (both amended 2026-09-08), `docs/http-direct-revolt-integration.md`

## Why

`pool.type = http-direct` (task 054) serves HTTP from inside an FPM worker, but
PHP runs inside the evhttp callback
(`sapi/fpmng/fpm/fpm_http_direct.c:508`, `:518`, `:412`, `:431`) and the event
base is unreachable from PHP. That shape supports exactly one request at a time
per worker and cannot host a userland event loop: a Revolt driver pumping the
same base from inside that callback hits libevent's reentrancy guard —
measured on the test box, libevent 2.1.12-stable returns -1 and warns
`event_base_loop: reentrant invocation`, so userland `await` would fail
outright.

`docs/http-direct-revolt-integration.md` argues the inverse arrangement: the
worker boots PHP once, the PHP script owns the loop, and the SAPI supplies
libevent primitives plus a request callback. Two claims follow from it that are
cheap to falsify and expensive to assume:

1. an unmodified `revolt/event-loop` can drive an FPM worker's libevent base
   through a driver written entirely in userland PHP;
2. once it does, concurrent requests per worker need no SAPI fiber executor —
   they are userland fibers.

This POC exists to test those two claims and nothing else.

## Scope

A second, experimental executor for `pool.type = http-direct` —
`pool.executor = worker` — in which the worker executes one script for its
whole lifetime instead of one script per request, and which exposes to
PHP: registration of fd/timer/signal watchers on the worker's event base, a
single-iteration loop pump with and without blocking, a per-request callback,
and a way to answer a request later than the callback that delivered it.

The Revolt driver itself is userland PHP shipped as an example, not C, and not
a bundled extension: the SAPI must stay unaware of Revolt.

Deliberately out of scope, to be recorded as documented limits rather than
fixed: per-request isolation, `echo`/`header()` mapping onto the current
request, scoreboard/status parity, TLS, streaming and HTTP/2.

## Acceptance criteria

- A dependency-free regression test in `sapi/fpmng/tests/` proves the loop
  primitives: on one worker, four requests each waiting one second on a worker
  timer overlap in time (last start before first end, as in
  `fpmng-fiber-sleep-concurrency.phpt`), and a plain request returns its body.
- A reproducible harness proves the same thing with **unmodified**
  `revolt/event-loop` and `amphp/amp` from Composer: N concurrent requests to a
  handler doing `Amp\delay(1)` complete in roughly one second in total, not N
  seconds, served by a single worker with `pm.max_children = 1`. The harness
  skips cleanly where Composer or network is unavailable.
- The example application is a hello world plus that one sleeping route, small
  enough to read in one screen.
- Both executors of `pool.type = http-direct` still pass the existing
  `sapi/fpmng/tests/` suite, and a configuration using `pool.executor = worker`
  without a worker script, or with a directive whose semantics the worker
  lifecycle breaks, is rejected in the master with a message naming the pool.
- Measured numbers and every limit hit are recorded in the Outcome section.

## Out of scope

- Making this a supported feature, documenting it in `README.md` as such, or
  offering migration guidance for existing applications.
- Any change to `pool.type = http`, `fastcgi`, `fastcgi-ng` or the existing
  `http-direct` classic request path. The only edit to shared code is the
  `extra_executor` / `extra_executor_type` pair in `fpm_pool_type_s`, so that
  `fpm_pool_type_resolve()` learns the new executor from data rather than from
  another type-name comparison.
- The fiber executor and its build gating.

## Outcome

Both claims hold. An unmodified `revolt/event-loop` drives the worker's
libevent base from a userland driver, and concurrent requests on one worker
need no SAPI fiber executor.

### What was done

- `sapi/fpmng/fpm/fpm_http_direct_worker.c` — the executor: one
  `php_request_startup()` per worker lifetime, the worker script owns the loop,
  and the SAPI exposes `fpmng_worker_loop()`, `fpmng_worker_next_request()`,
  `fpmng_worker_request_env()`, `fpmng_worker_respond()`,
  `fpmng_worker_event_create/enable/disable/free()`,
  `fpmng_worker_notify_stream()` and `fpmng_worker_stopping()`.
- Modelled as an **executor, not a second pool type**: `pool.type =
  http-direct` with `pool.executor = worker`, resolved by
  `fpm_pool_type_resolve()` from the new `extra_executor` /
  `extra_executor_type` data pair — the only edit to shared per-type code.
  Same transport, same listener, same master-side bookkeeping; only the child
  loop is inverted, which is exactly why `fiber` is an executor too.
- `fpm_pool_type_check_directives()` now names the configured executor in its
  rejection message. Without it a directive that plain `pool.executor =
  classic` accepts was reported as "not supported by pool.type = http-direct".
- `examples/http-direct-worker/` — the userland Revolt driver (`FpmngDriver`
  extends `Revolt\EventLoop\AbstractDriver`), an HTTP glue class, and an
  `app.php` that is hello world plus one `/sleep` route doing `Amp\delay(1.0)`.
- Two test layers: `sapi/fpmng/tests/fpmng-http-direct-worker.phpt`
  (dependency-free, core `Fiber` on the raw primitives, runs in CI) and
  `build/test-http-direct-amphp.sh` (real Composer amphp, SKIPs cleanly).
- Four rejection cases added to `fpmng-config-rejected-directives.phpt`.

### What was measured (test box 192.168.8.50, php-8.5.9, `--disable-all
--enable-fpmng --enable-session --with-openssl`)

Binary under test confirmed first: `strings` on the built `php-fpm-ng` gives
exactly one hit for `php-fpm-ng/http-direct-worker`.

- Full `.phpt` suite: **PASS 15 / FAIL 0 / SKIP 8**. The eight skips are the
  fiber tests — this binary is built without `--enable-fpmng-fiber`.
- amphp harness, `amphp/amp v3.1.3` + `revolt/event-loop v1.0.9`, both
  unmodified: `hello-world: ok`, `concurrent-delay: ok (8 requests in 1s on one
  worker)`, `single-worker: ok`, `PASS`.
- Re-run at N=64: `concurrent-delay: ok (64 requests in 1s on one worker)`,
  `single-worker: ok`, `PASS`. 64 × `Amp\delay(1.0)` on one worker still
  finishes inside the 3 s budget.
- Raw per-request evidence, one worker pid 2663926, 8 requests: `t0` spread
  **1.5 ms** (1788863218.659050 .. 1788863218.660597), every `t1` about
  **1.000 s** later (1788863219.659801 .. 1788863219.660217). Eight one-second
  delays in one second, not eight.
- Harness skip path verified with no Composer and no prepared `vendor/`: exits
  0 with `SKIP: no PHP CLI to run Composer with`.

### Limits hit and left in place

- Composer cannot run against our own CLI on the box: "PHP's phar extension is
  missing" under `--disable-all`. `vendor/` was built on the dev machine
  (PHP 8.5.10, Composer 2.10.3) and copied over; the harness takes it through
  `FPMNG_AMPHP_VENDOR`. Consequence: the amphp layer is **not** gated in CI —
  recorded as a finding.
- Eight fiber tests were not run (no `--enable-fpmng-fiber` in this build), so
  the claim "both executors still pass the suite" is verified for `classic` and
  `worker`, **not measured** for `fiber`.
- Documented, unfixed by design: no per-request isolation (one PHP request
  lifetime for the whole worker, so `max_execution_time` must be 0), one
  blocking call stalls every in-flight request on that worker, no signal
  watchers, no `echo`/`header()` mapping onto the current request, no
  scoreboard/status parity, no TLS, streaming or HTTP/2.

### Review findings fixed before the PR

A Bugbot review of the branch (workflow.md step 5) found seven major issues in
this branch's own new code. All are fixed here, and the tests were re-run after
the fixes (`PASS=15 / FAIL=0 / SKIP=8`, amphp harness `PASS`):

1. **Shutdown use-after-free.** `evhttp_free()` ran after
   `zend_hash_destroy(&fw.pending)`. It closes still-open connections and does
   fire their close callbacks — measured against libevent 2.1: "before
   evhttp_free, got_closecb=0" / "CLOSECB fired" / "after evhttp_free,
   got_closecb=1" — and each callback writes `p->http = NULL` through a freed
   `struct fpm_worker_pending`. `evhttp_free()` now runs first.
2. **Permanent 503.** A handler that returned without answering burned its
   pending slot for the worker's life, and after `FPM_WORKER_PENDING_MAX` such
   requests every later request got 503 forever with nothing to recover it
   (`request_terminate_timeout` is rejected, `pm.max_requests` counts only
   answered requests). Saturation now asks for a graceful stop so the master
   respawns the child, and logs why. The example answers from a `finally`.
3. **SIGQUIT handler installed before the notify pipe existed**, contradicting
   the comment that said the opposite; a signal in that window wrote to fd 0.
   The pipe is created first and both ends start at -1.
4. **Watcher callback released mid-call.** Freeing a watcher from inside its own
   callback is the advertised cancellation idiom, so `call_user_function()` ran
   on a zval that had just been destroyed. A `Closure` survived by accident
   (`zend_call_function()` addrefs it); an `[$obj, 'method']` callable did not.
   The callback is copied for the duration of the call.
5. **Watcher held an fd but not the stream.** Closing the stream closed the fd
   under an enabled watcher, and a long-lived worker reuses descriptor numbers.
   The watcher now keeps a reference to the stream zval.
6. **`fpmng_worker_respond()` accepted 1xx**, which `evhttp_send_reply()` emits
   as if it were final and still frames a body behind. Restricted to 200..599,
   matching the classic transport (`fpm_http_direct.c:454-459`).
7. **`evhttp_add_header()`'s return value was dropped**, so a rejected or
   over-budget header vanished silently. It is now a 500 and a `false` return,
   plus an RFC 9110 token check on the name. There is no response-splitting
   vector either way: libevent 2.1 already rejects CR/LF in key and value
   (measured). Covered by a new `malformed-header: ok` assertion in the
   `.phpt`.

The three `findings.md` entries the review confirmed as task-worthy (shared
code duplicated between the two executors; `fpm_pool_type_resolve()` still
`strcmp`ing for `fiber`/`async`; the amphp harness ungated in CI) are left as
follow-ups, not bundled into this PR.
