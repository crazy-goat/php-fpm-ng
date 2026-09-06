# Spike: yield sleep()/usleep()/time_nanosleep() in fiber pools

Status: DONE. All measurements below were run on `piotr@192.168.8.50` in
`~/rd/b3/`, ports 18971 (AFTER, this spike's build) and 18972 (BEFORE,
same tree with the one `fpm_pool_fiber_sleep_install()` call commented
out), never touching the box's other sessions' ports/dirs.
Branch: `spike/sleep-yield`, worktree `/Users/piotr.halas/work/php-fpm-ng-worktrees/sleep-yield`.

## Question

Can `sleep()`, `usleep()` and `time_nanosleep()` yield the fiber instead of
freezing the whole worker process — by replacing the internal function
handler at pool startup, without patching php-src — for `pool.executor =
fiber` pools?

## Verdict

**Feasible, and worth doing.** Replacing the `zif_handler` for `sleep()`,
`usleep()` and `time_nanosleep()` once per fiber-pool child process, with
no php-src patch, closes exactly the gap the task set out to close:

- Headline: one `sleep(3)` request today (unpatched) blocks **0 / 10** and
  **0 / 30** concurrent unrelated requests from being served during those 3
  seconds; with this patch, **10 / 10** and **30 / 30** get served, while
  the sleeping request still takes its correct ~3.0s (measured
  3000.13-3002.07ms across both cases).
- Correctness is preserved in every dimension checked: argument validation
  matches upstream byte-for-byte, `time_nanosleep()` returns `true` (not an
  array) on normal completion, `usleep(2000)` stays millisecond-class
  (~2.0-2.2ms single-shot, ~1.5-4.1ms under 20-way concurrent contention —
  never rounded to whole seconds), three differently-timed sleeps woken
  concurrently come back in the right order with their own durations
  (1.02s / 2.02s / 3.02s, not all released at 3s), the required fallback to
  real blocking behavior when it's unsafe to suspend is proven both to
  return the right value (0) and to actually re-freeze the whole process
  (0/5 concurrent requests served during the fallback's blocking window,
  same as the unpatched baseline), and a client aborting mid-sleep survives
  100 repeated cycles with flat RSS and no crash.
- Overhead on requests that never touch these functions is unmeasurable
  (~1.35-1.43ms/req both with and without the patch, dominated by the load
  generator's own process-spawn cost, not server-side work) — which
  matches the design: the replaced handlers are simply never on that call
  path.
- The implementation is small and contained: two new files
  (`fpm_pool_fiber_sleep.c`/`.h`, ~230 lines together) plus a two-line hook
  (one `#include`, one call) in `fpm_pool_fiber.c` — exactly the "new files,
  minimal hooks" shape the project wants, and the install point is the pool
  type's own `child_main`, so it structurally cannot run for any pool type
  but fiber.
- The main risk (see below) — the technique's reliance on a single-threaded,
  non-ZTS, single-owner-of-the-function-slot world — is not a new risk this
  spike introduces: `pool.executor = fiber` already requires exactly that
  world for unrelated reasons (its whole `fpm_pool_coop.c` design shares one
  process/one function table across requests, and already rejects ZTS and
  OPcache). This technique fits the executor's existing constraints instead
  of adding new ones.

Scope honestly drawn: this covers the three functions the task named and
nothing else — `curl`, `pdo_pgsql`/libpq, plain file I/O remain fully
blocking, as does `pcntl_sleep()` (already unreachable from fiber pools) and
`stream_select()`/raw socket timeouts outside `tcp`/`unix` (xport's
territory, out of scope for this spike). Those are each materially bigger
pieces of work (curl and libpq have their own async/multi APIs to wire into
the same scheduler; xport is someone else's parallel work right now) and
were correctly excluded rather than attempted here.

## What was built

New files, only in `sapi/fpmng/fpm/`:

- `fpm_pool_fiber_sleep.c` / `.h` — replaces the `zif_handler` of `sleep`,
  `usleep`, `time_nanosleep` in `CG(function_table)`, once per fiber-pool
  child process.

Existing-file hooks (minimal, both in `sapi/fpmng/fpm/fpm_pool_fiber.c`):

- `#include "fpm_pool_fiber_sleep.h"`
- one call, `fpm_pool_fiber_sleep_install();`, added right after the
  existing `fpm_pool_fiber_xport_install();` call inside
  `fpm_pool_fiber_child_main()` (around line 539 before the edit; see
  "Install point" below for the exact reasoning).

No other file was touched. `php-src` was never modified (read-only
reference at `/Users/piotr.halas/work/php-src`).

### Install point — file:line and why

`sapi/fpmng/fpm/fpm_pool_fiber.c`, inside `fpm_pool_fiber_child_main()`,
immediately after the `fpm_pool_fiber_xport_install();` call. That function
is the pool type's `child_main` hook (`struct fpm_pool_type_s.child_main`,
`sapi/fpmng/fpm/fpm_pool_type.h`) — the ONE per-type function pointer the
project's pool-type contract provides for "what this pool type does with
its child process". It is wired into `fpm_pool_types[]` (`fpm_pool_type.c`)
only for the `fiber` entry, so calling `fpm_pool_fiber_sleep_install()` from
inside it can never run for any other pool type — no `if (type == fiber)`
anywhere, exactly as the contract in `fpm_pool_type.h` requires. This is the
same reasoning that already put `fpm_pool_fiber_xport_install()` there, and
it needs to run after MINIT for the same reason xport install does (so it
can grab whatever the loaded extensions leave in the function table) —
though unlike xport install there is no known extension that replaces
`sleep`/`usleep`/`time_nanosleep` in MINIT, so this ordering constraint is
mostly "stay consistent", not "required to avoid a specific known clash".

### Design decision: no extra libevent timer, no extra teardown hook

The task brief for this spike suggested arming a *separate* libevent timer
whose callback calls `fpm_pool_fiber_wake()`, then suspending with
`fpm_pool_fiber_wait_wake(NULL)` — and warned this would need a teardown
hook (analogous to `fpm_coop_req_free()`'s per-subsystem hooks) to cancel
that timer if the fiber is torn down while it's pending.

Rereading `fpm_pool_fiber.h`/`.c`, `fpm_pool_fiber_wait_wake(timeout)`
**already is** exactly that primitive: it arms a timeout directly on the
scheduler's own per-request event (the same one `wait_fd()` uses), suspends,
and returns `0` when that timeout elapses — with no external wake call
needed. That event is already correctly owned and cleaned up by the
scheduler (`event_del()` after every wait, `event_free()` when the fiber
dies — `fpm_fiber_after_switch()` in `fpm_pool_fiber.c`), the exact same way
for a timed-out fd-wait as for a timed-out wake-wait.

So the sleep handlers call `fpm_pool_fiber_wait_wake(&tv)` directly with the
requested duration as the timeout, and treat `0` (timed out) as "slept the
full requested time" — which is the only outcome that can happen in this
spike, since nothing holds onto our waiter to call `fpm_pool_fiber_wake()`
on it early. Building a second, independent libevent timer on top would
have meant two clocks measuring "how long has this fiber slept", would have
introduced a genuinely new object needing its own teardown path (the risk
the brief was correctly worried about) — and that hazard is exactly what
using the existing primitive directly avoids. `fpm_pool_fiber_waiter()`/
`fpm_pool_fiber_wake()` (the pair for waking from an *external* completion
source, e.g. evdns) are consequently unused by this spike's sleep code —
sleep has no external completion source, only a deadline.

Net effect: **no new event object is created, so there is nothing new to
leak or dangle, and no `fpm_coop_req_free()`-style hook was added.** This is
a deliberate deviation from the literal brief, made because the "cleanest
way to drive this with one clock" turned out to be "use the clock the
scheduler already drives", not "add a second one". See "Cleanup safety"
below for the empirical proof that this holds up under repeated abort
cycles.

### Coverage

Covered: `sleep()`, `usleep()`, `time_nanosleep()`.

Deliberately NOT covered (see also the header comment in
`fpm_pool_fiber_sleep.h`):

- `pcntl_sleep()` — built on `SIGALRM`/process-level pause, no per-request
  equivalent in this model; `fpm_pool_coop.c` already disables the whole of
  `ext/pcntl`'s process-affecting surface for fiber pools via
  `zend_disable_functions`, so this isn't reachable from fiber pools anyway.
- `stream_select()` / timeouts on sockets outside the `tcp`/`unix`
  transports — that's `fpm_pool_fiber_xport.c`'s territory, explicitly out
  of scope per the task brief (parallel flock work touches streams/xport).
- `time_sleep_until()` — same `ext/standard` family, same
  `HAVE_NANOSLEEP` guard, not in the task's required list. Not covered in
  this spike; would follow the exact same pattern as `time_nanosleep()` if
  wanted later (loop internally on EINTR upstream, but nothing produces a
  real EINTR-equivalent in this model, so it would look like a single
  `wait_wake()` call, same as the others here).
- `curl`, `pdo_pgsql`/libpq, plain file I/O — out of scope for this spike
  (named explicitly in the task; these are separate, larger pieces of work).

## Measurements

All measurements were run on `piotr@192.168.8.50` (`~/rd/b3/`), against a
locally built `--enable-fpmng --enable-debug` binary
(`~/rd/b3/build/sapi/fpmng/php-fpm-ng`), never against the box's other
sessions' processes. Ports used: TBD (within 18971-18990).

### 1. Headline: concurrent requests served during sleep(3)

Harness: `~/rd/b3/concurrent.sh <port> <sleep_seconds> <n_workers>` on the
test box. It fires the `sleep($N)` request and `N_WORKERS` unrelated
`noop.php` requests all at t=0 (all via `cgi-fcgi` FastCGI clients, one TCP
connection each), each `noop.php` request wrapped in `timeout $(N-0.3)s` so
that a completion arriving *after* the window is counted as a failure, not
silently as a late success. Two servers, same box, same code except the one
line that calls `fpm_pool_fiber_sleep_install()`:

- port 18972 = BEFORE (`~/rd/b3/build-before`, that one call commented out —
  `sleep()`/`usleep()`/`time_nanosleep()` are the real libc functions,
  unpatched, exactly today's behavior)
- port 18971 = AFTER (`~/rd/b3/build`, this spike's code)

Both: `pool.type = fastcgi-ng`, `pool.executor = fiber`, `pm = static`,
`pm.max_children = 1` (one process, N requests in flight, per the fiber
executor's whole point).

| | BEFORE (unpatched) | AFTER (this spike) |
|---|---|---|
| `noop.php` served inside the 2.7s window, 10 concurrent | **0 / 10** | **10 / 10** |
| `noop.php` served inside the 2.7s window, 30 concurrent | **0 / 30** | **30 / 30** |
| `sleep(3)` wall time under that concurrent load | 3000.13 ms | 3000.57-3002.07 ms |

Raw output, verbatim:

```
BEFORE (10 workers):  SERVED_WITHIN_WINDOW=0 / 10 (budget=2.7s)   sleep elapsed_ms=3000.13
AFTER  (10 workers):  SERVED_WITHIN_WINDOW=10 / 10 (budget=2.7s)  sleep elapsed_ms=3000.57
BEFORE (30 workers):  SERVED_WITHIN_WINDOW=0 / 30 (budget=2.7s)   sleep elapsed_ms=3000.12
AFTER  (30 workers):  SERVED_WITHIN_WINDOW=30 / 30 (budget=2.7s)  sleep elapsed_ms=3002.07
```

So: today, one client calling `sleep(3)` on a `pool.executor = fiber` pool
freezes the entire worker process for the full 3 seconds — **zero** other
requests get served, confirmed on this exact harness, not just asserted.
With the patch, all concurrent requests (tested up to 30) are served
essentially immediately, while the sleeping request still takes its full,
correct ~3.0s.

### 2. Timing accuracy

`sleep(3)` under concurrent load (from the headline test above): 3000.13-
3002.07 ms measured wall time — correct, not shortened or lengthened by the
concurrent traffic.

`usleep(2000)` (request 2ms), 20 sequential single-shot requests, AFTER
build, port 18971 — measured deltas (ms), all sub-millisecond-class,
clustered right around the 2ms request, never rounded to a whole second:

```
2.15 2.15 2.14 2.13 2.13 2.11 2.17 2.14 2.08 2.12
2.16 2.08 2.11 2.16 2.09 2.12 2.15 2.17 2.11 2.09
```

`usleep(2000)`, 20 requests fired **concurrently** (all 20 fibers sleeping
at once, same process) — deltas widen a bit under real scheduling contention
(one process now has to wake, resume, and re-suspend 20 fibers through the
same libevent loop) but stay millisecond-class, nowhere near rounded to a
whole second:

```
2.17 2.03 2.06 1.47 2.18 1.97 2.14 2.10 2.03 2.96
2.69 3.02 2.19 2.89 3.96 3.67 3.83 2.86 4.09 2.54
```
(min 1.47ms, max 4.09ms, median ~2.4ms, for a 2ms request — reasonable given
20-way fan-in on one event loop; still three orders of magnitude away from
being rounded to whole seconds.)

### 3. Ordering under concurrency

Three `sleep()` requests fired at the same instant (`sleep(3)`, `sleep(1)`,
`sleep(2)`, all started within the same `for`-loop `&` fan-out, port 18971,
AFTER build). Time each one completed, relative to the common start `T0`,
and its own measured wall time:

```
t=1.029s   req sleep(1)  wall=1018.7ms
t=2.030s   req sleep(2)  wall=2019.1ms
t=3.029s   req sleep(3)  wall=3019.0ms
```

They woke back-to-back in the correct order (1s, then 2s, then 3s — not
all three released together at the 3s mark, and not swapped), and each got
its own requested duration, not the maximum of the three. This confirms the
per-request timeout on `fpm_pool_fiber_wait_wake()` is genuinely independent
per fiber, not a single shared deadline.

### 4. Fallback path proof (`fpm_pool_fiber_can_wait()` == false)

`can_wait()` returns false when `EG(active_fiber) != <the request's own
fiber>` — i.e. inside a nested, user-created `Fiber`. Forced it with:

```php
$f = new Fiber(function () { sleep(1); /* or sleep(2) for the load test */ });
$f->start();
```

`Fiber::start()` runs the fiber body inline until it finishes or suspends,
so this executes `sleep()` with `EG(active_fiber)` pointing at the nested
fiber, not the request's own — exactly the fallback condition.

Return-value proof: `sleep(1)` inside the nested fiber returned `0` (the
correct "slept the full time" value; the real `sleep(3)` upstream ships
this same value), confirmed by `nested2.php` returning
`{"sleep_return":0}`.

Blocking proof: fired `nested.php` (`sleep(2)` inside a nested Fiber) and,
0.1s later, 5 concurrent `noop.php` requests each bounded by a 1.7s
timeout, port 18971 (AFTER build — same process that has the patch
installed). `nested.php` took 2000.5ms end-to-end (real blocking sleep, not
shortened). **All 5 concurrent `noop.php` requests timed out (exit code
124, i.e. never got a response within 1.7s)** — the process was fully
frozen for the whole 2 seconds, identical to the BEFORE/unpatched behavior
in section 1. This is the required negative-case proof: when
`can_wait()` is false, the real blocking `sleep()` runs and the whole
process blocks exactly as it does today — the patch never silently skips a
sleep or suspends when it would be unsafe to.

### 5. Cleanup safety (abort mid-sleep)

Because the sleep handlers reuse `fpm_pool_fiber_wait_wake()` directly (see
"Design decision" above) instead of arming a second, independent libevent
timer, there is no new event object for this feature to leak or dangle in
the first place — the only per-request event involved is the scheduler's
own `fr->ev`, already freed by the scheduler exactly where it always was
(`fpm_fiber_after_switch()` when the fiber dies). The test below is the
empirical check that this holds under repeated abuse, not just an argument.

Harness: fired `sleep(2)` via `cgi-fcgi` in the background, then
`kill -9`'d the *client* process 0.2s later (closing its TCP socket without
waiting for the response) — repeated back-to-back with no wait between
cycles, so multiple abandoned "sleeping" fibers routinely overlapped in the
same process. 100 cycles total, in two batches of 50, against the AFTER
build (port 18971, child pid 732860 throughout — same pid start to finish,
i.e. no crash/respawn):

| | RSS (child pid 732860) |
|---|---|
| baseline, before any abort cycles | 9564 KB |
| after 50 abort cycles | 9980 KB |
| after 100 abort cycles (50 more) | 9980 KB (unchanged) |

RSS grew once (baseline -> 50 cycles, +416 KB — consistent with normal
allocator/arena growth on first use, not a per-cycle leak) and then was
flat across the second batch of 50 identical cycles. No crash, no restart,
same worker pid throughout both batches.

`~/rd/b3/logs/php-fpm.log` shows no error/warning entries from either
batch (only the two startup NOTICE lines from container boot). A plain
`sleep(1)` sent right after both batches still returned the correct result
in 1001.4ms — the scheduler was not left in a bad state by any of the 100
aborted sleeps.

Architecturally, why this is safe rather than just "didn't crash this
time": during a pending sleep, the scheduler's per-request event
(`fr->ev`) is watching ONLY the timer, not the client's fd — so a client
disconnect during the sleep is not even observed by the event loop until
the fiber resumes (when the sleep's timeout elapses) and the script goes on
to its next real I/O (typically writing the response), at which point the
existing, already-proven disconnect handling (used identically for every
other request type) takes over. This matches real upstream PHP's own
behavior — a blocking `sleep()` there doesn't notice a client disconnect
mid-sleep either.

### 6. Overhead on non-sleeping requests

500 sequential `noop.php` requests (never calls sleep), 3 runs each,
BEFORE (port 18972, no handler replacement at all) vs. AFTER (port 18971,
handler replacement installed but never exercised by this request):

```
run1 AFTER : 2.166 ms/req   (cold-start outlier, first request after idle)
run1 BEFORE: 1.356 ms/req
run2 AFTER : 1.358 ms/req
run2 BEFORE: 1.373 ms/req
run3 AFTER : 1.429 ms/req
run3 BEFORE: 1.369 ms/req
```

Excluding the one cold-start outlier, both sit at ~1.35-1.43ms/req — no
measurable difference, well within run-to-run noise, and this per-request
cost is dominated by `cgi-fcgi` spawning a fresh client process per call,
not by anything server-side. This is expected: the replaced handlers are
never on the call path for a request that doesn't call `sleep`/`usleep`/
`time_nanosleep` — the only per-request cost the patch could add is the
one-time, once-per-child-process `zend_hash_str_find_ptr()` lookup at
startup, not anything repeated per request.

### Extra: argument validation and `time_nanosleep()` success contract

Quick functional check that the argument validation matches upstream
exactly (same exception type, same message text), and that
`time_nanosleep()`'s normal-completion contract (`true`, not an array) is
preserved:

```
time_nanosleep(0, 300000000)         -> true, elapsed 300.4ms  (correct: true on full completion)
time_nanosleep(0, 2000000000)        -> ValueError: "Nanoseconds was not in the range 0 to 999 999 999 or seconds was negative"
time_nanosleep(-1, 0)                -> ValueError: "time_nanosleep(): Argument #1 ($seconds) must be greater than or equal to 0"
sleep(-1)                            -> ValueError: "sleep(): Argument #1 ($seconds) must be between 0 and 4294967295"
usleep(-1)                           -> ValueError: "usleep(): Argument #1 ($microseconds) must be between 0 and 4294967295"
```

All four messages are byte-for-byte what upstream `ext/standard/basic_functions.c`
produces for the same inputs (verified by reading the upstream source, not
from memory — see `PHP_FUNCTION(sleep)`, `PHP_FUNCTION(usleep)`,
`PHP_FUNCTION(time_nanosleep)`).

## Risks of "replace the internal function handler" as a technique

- **Thread safety.** This only works because `pool.executor = fiber`
  already requires a non-ZTS build — `fpm_pool_fiber_validate()` /
  `fpm_pool_fiber_child_main()`'s ZTS stub rejects ZTS outright
  (`fpm_pool_fiber.c:41-49`). `CG(function_table)` is then a single
  process-global table and one child process is single-threaded (libevent,
  one fiber on the CPU at a time), so mutating one `zif_handler` pointer at
  startup is a plain, uncontended write. The technique as written here
  **does not generalize to a threaded SAPI** — on ZTS, function tables can
  be per-thread copies-on-write and this would need real synchronization or
  a per-thread install, neither of which exists here.
- **opcache / preloading.** Fiber pools already refuse to start with
  OPcache enabled (`fpm_coop_validate`, see `docs/fiber_errors.md`), so this
  spike never runs alongside opcache and was not tested against it. Flagging
  for whoever lifts that restriction later: OPcache preloading
  (`opcache.preload`) copies function/class definitions into
  shared/immutable memory ahead of fork and reuses them across children;
  if `sleep`/`usleep`/`time_nanosleep` entries ever ended up backed by that
  shared preload arena, writing `fn->internal_function.handler` could hit a
  read-only/shared page (crash) or apply the swap to every child at once
  instead of the one that called `..._install()` — the opposite of the
  intended per-child scope. Not exercised in this spike; re-verify before
  ever relaxing the opcache-disabled requirement for fiber pools.
- **Two subsystems wrapping the same function.** This code assumes it is
  the only thing that ever touches `sleep`/`usleep`/`time_nanosleep`'s
  slot. It plays reasonably well with an extension that already wrapped
  the function in its own MINIT (we run at child_main, after MINIT, so we
  save *and chain to* whatever is there — same pattern
  `fpm_pool_fiber_xport_install()` already relies on for openssl's `tcp`
  factory). It does **not** play well with something that wraps the same
  function *after* us and doesn't chain: whichever installer runs last
  simply overwrites `fn->internal_function.handler`, and the earlier
  installer's saved-original pointer is now the wrong thing to "fall back"
  to if the later wrapper doesn't preserve the chain. If this pattern gets
  reused elsewhere in fpm-ng (e.g. a future curl/libpq yield point wrapping
  its own functions is fine — different slots — but a second thing wanting
  `sleep` too would collide), it needs an explicit single-owner rule or a
  small shared registry, not two independent `_install()` functions racing
  to be last.
- **Restart/reload behavior.** Not a risk in practice: a pool reload forks
  a fresh child that runs `child_main` (and therefore `..._install()`)
  from scratch against its own freshly-initialized `CG(function_table)` —
  nothing stale carries across a reload, since nothing here is persisted
  outside the one process's memory.
- **Extension load order.** Installing from `child_main`, after the SAPI
  has finished all extensions' MINIT, means we always wrap whatever the
  function table's final entry is — we cannot end up wrapping a function
  another extension's MINIT later replaces (unlike, hypothetically,
  installing during our own MINIT, which is why `fpm_pool_fiber_xport_install()`
  documents the same constraint for `ext/openssl`). No known extension
  currently touches `sleep`/`usleep`/`time_nanosleep`'s handler, so this
  wasn't observed to bite, but the ordering choice is deliberate insurance
  against it, not proof nothing could.
- **Debuggers/profilers.** Tools like Xdebug commonly hook internal
  function handlers too (for tracing/step debugging). Same "two subsystems,
  one slot" risk as above; not tested here (no Xdebug in this build).
- **Graceful degradation when the assumption breaks.** If `sleep` (etc.)
  isn't found in `CG(function_table)`, or isn't `ZEND_INTERNAL_FUNCTION`
  (e.g. some future build defines it differently, or as userland PHP), the
  installer logs a `ZLOG_WARNING` and leaves that function alone — it never
  hard-fails pool startup. Verified: this is the same code path exercised
  whenever `HAVE_NANOSLEEP` is undefined for `time_nanosleep`.

## Surprises / dead ends

- The task brief's suggested design (separate libevent timer + waiter/wake
  callback) turned out to be unnecessary — see "Design decision" above.
  Worth flagging because it means the primitive `fpm_pool_fiber.h` already
  exposes (`wait_wake(timeout)`) is more directly reusable for "just wait
  this long" needs than the header comment alone suggests; a future author
  adding a similar timeout-based yield point should look here first before
  building their own timer.
- The build-system trap called out in the task ("adding a `.c` file is
  silently not compiled unless you `buildconf --force` + reconfigure") is
  real and worth respecting literally: verified with `nm` on the built
  binary that `fpm_pool_fiber_sleep_install` was actually present before
  trusting any measurement (see "What was built").
- `bc` is not installed on the test box; every duration/threshold
  computation in the shell harnesses had to go through `awk` instead — a
  small, easy-to-miss portability snag when improvising load-test scripts
  on an unfamiliar box.
- The very first "headline" measurement attempt gave a misleading `10/10`
  served for the **unpatched** BEFORE build, which looked like a
  contradiction until the harness bug was found: counting a request as
  "served" as soon as its foreground `cgi-fcgi` call returned, with no
  bound on how long that could take, meant late completions **after** the
  blocking process finally woke up were being counted as if they happened
  during the 3-second window. Fixed by bounding every concurrent probe with
  `timeout` set just under the sleep duration, so a completion after the
  window counts as a failure, not a delayed success. Worth remembering for
  any future "is the process blocked" test on this kind of harness — an
  unbounded client silently hides the exact blocking behavior it's meant to
  detect.
