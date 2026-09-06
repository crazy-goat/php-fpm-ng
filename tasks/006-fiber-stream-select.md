# 006 — Make `stream_select()` cooperate with the fiber executor

**Priority:** medium-high. Blocks two common libraries outright.
**Status:** open.

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
