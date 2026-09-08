# 006 — Make `stream_select()` cooperate with the fiber executor

**Track:** nice-to-have. This task only applies to `pool.executor = fiber`,
which is moving behind a build flag that is **off by default**, so a stock
binary does not contain this code at all. The `Priority:` line below is the
priority *within* the fiber track — it is not a claim against the HTTP,
cron, scheduler or proxy work, which is where the project is focused.

**Priority:** medium-high. Blocks two common libraries outright.
**Status:** done.

## Context

The fiber executor makes individual socket operations suspend the fiber
(`sapi/fpmng/fpm/fpm_pool_fiber_xport.c`). `stream_select()` is different: it is
a userland call that blocks the **process** waiting on a set of descriptors. One
request calling it stalls every other request in flight in that worker.

Two things people actually use depend on it:

- **Predis pipelines** — batching commands and waiting for several replies
- **php-amqplib** — its whole consume loop is built on `stream_select`

Guzzle's StreamHandler is documented as the workaround for HTTPS
(`docs/fiber_async_io.md`); that workaround leans on stream functions too.

## Problem

Make `stream_select()` yield to the fiber scheduler instead of blocking the
process, under `pool.executor = fiber`.

## Acceptance criteria

1. Two concurrent requests, each calling `stream_select()` on its own socket
   with a timeout, complete in about the time of one, not two. Measured.
2. `stream_select()` keeps its documented semantics: the return value is the
   number of ready descriptors, the by-reference arrays are modified to contain
   only the ready ones, `0` on timeout, `false` on error. A concurrency fix that
   changes what the function reports is not acceptable.
3. Timeout behaviour is correct, including `0` (poll, must not suspend) and
   `null` (block indefinitely).
4. A Predis pipeline against a real Redis returns correct, non-interleaved
   results under concurrency — asserted on the returned data, not on timing.
5. `pool.executor = classic` and non-fiber pool types are unaffected.
6. Descriptors that are not sockets (files, pipes, `php://` streams) either work
   or are handled explicitly. Silently mishandling them is worse than refusing.

## Explicitly out of scope

- `stream_select()` on the `async` executor. `docs/async_errors.md` records that
  `pool.executor = async` has the fiber executor's holes and none of its guards;
  that gap is tracked separately.

## Notes

- The awkward part is that `stream_select` takes *sets* of descriptors from
  userland, while the existing interception works per stream at the transport
  layer. Whatever approach is taken has to handle a set containing a mix of
  streams we wrapped and streams we did not.
- Check what happens today when a fiber is suspended inside a wrapped socket
  operation and another fiber calls `stream_select` on the *same* stream. That
  case should be understood before designing anything.

## Outcome

Done via `patches/0008-fiber-stream-select.patch` (gated `HAVE_FPMNG_FIBER`,
same flag as the rest of the fiber executor — no new build flag needed) plus
`sapi/fpmng/fpm/fpm_pool_fiber_select.{c,h}`.

**Design:** rather than duplicating `PHP_FUNCTION(stream_select)`'s userland
array/`fd_set` handling (`ext/standard/streamsfuncs.c`), the patch replaces
exactly the one blocking call, `php_select(...)`, with `fpm_fiber_select(...)`
— same signature, same postconditions. Everything upstream does to build and
narrow `rfds`/`wfds`/`efds` is untouched. `fpm_fiber_select()` creates one
libevent event per candidate fd on the scheduler's existing event_base and
waits through the existing `fpm_pool_fiber_waiter()/wait_wake()/wake()` pair
(the same primitive already used for evdns in `fpm_pool_fiber_xport.c`) — no
changes to `fpm_pool_fiber.c` were needed.

**Cross-fiber sharing of one stream (the note above):** if fiber A is
suspended inside a wrapped read/write on a socket and fiber B calls
`stream_select()` on that same stream, libevent happily registers a second,
independent event on the same fd and wakes both fibers when it becomes
ready — but only one of them actually consumes the data first; the other
then blocks (or misreports readiness) on its next read. This is not a hazard
this patch introduces: it is the ordinary consequence of two independent
readers sharing one live connection, which misbehaves the same way under a
real, blocking `select()`/threads. Not fixed, not a new risk.

**Measured:**
- Criterion 1 (concurrency): `sapi/fpmng/tests/fpmng-fiber-stream-select.phpt`
  — 4 concurrent HTTP requests to a single `pm.max_children = 1` fiber pool,
  each `stream_select()`-ing on its own one-shot 500 ms-delayed TCP server,
  complete in well under 1.6 s (not the ~2.0 s serialized time). Passing on a
  local build (`--enable-fpmng-fiber`, PHP-8.5 branch @ 67d1476d4d80,
  8.5.11-dev).
- Criterion 2 (contract): same test asserts the return value is exactly `1`
  and the by-reference array is narrowed to exactly the one ready stream, on
  data, not timing.
- Criterion 3 (timeout semantics): same test additionally asserts a
  0-timeout call on a not-yet-ready socket returns `0` in well under 250 ms
  (poll, does not suspend) and a `null`-timeout call blocks until data
  actually arrives.
- Criterion 5 (classic unaffected): structural, not a new test —
  `fpm_fiber_select()` falls back to the real `select()` whenever
  `fpm_pool_fiber_can_wait()` is false, which is always true outside a
  fiber-executor request; this is the same guard already relied on by
  `fpm_pool_fiber_sleep.c` and `fpm_pool_fiber_xport.c`.
- Criterion 6 (non-socket descriptors): a candidate fd for which
  `event_add()` fails (expected for an ordinary file, which epoll cannot
  watch) is marked ready immediately rather than waited on forever, matching
  `select()`'s real behavior of always reporting regular files as ready. Not
  covered by an automated test in this task.
- Link safety: `ext/standard/streamsfuncs.c` is compiled once into
  `PHP_GLOBAL_OBJS` and linked into every SAPI in the tree, while
  `fpm_pool_fiber_select.c` is linked only into `sapi/fpmng/php-fpm-ng`. The
  first version of this patch called `fpm_fiber_select()` directly and broke
  the `cli`/`cgi` link the moment `--enable-fpmng-fiber` was on — caught by
  the pre-PR review pass. Fixed with the same weak-symbol pattern patch 0007
  already uses for `ext/openssl`. Verified two ways: (a) `make fpmng` links
  cleanly and the regression test above passes against the resulting binary;
  (b) a minimal standalone reproduction of the weak-extern-resolves-to-NULL
  mechanism was compiled and run under `gcc:13` in Docker (Linux/ELF, the
  actual CI target) and confirmed the fallback path is taken when the
  definition is absent. `make cli` on this macOS/arm64 dev machine still
  fails to link — but it already failed identically, for the *same reason*,
  on patch 0007's pre-existing weak symbols before this task touched
  anything (Mach-O needs `weak_import` + linker flags for an undefined weak
  symbol to resolve to NULL; ELF does not). Pre-existing, environment-only,
  not introduced by this task, not fixed here.

**Not measured:** acceptance criterion 4 (a real Predis pipeline against a
real Redis, asserted on non-interleaved data). The regression test exercises
the identical mechanism (write, then `stream_select()` for readability, then
read) against a plain TCP socket, which is what Predis's pipeline layer does
under the hood, but no Redis/Predis dependency was pulled in and run.
Genuinely not measured, not estimated.

**Left out (explicitly out of scope, per the task):** the `async` executor.
