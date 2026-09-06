# 021 — `curl` blocks the fiber executor

**Priority:** low. Deferred by decision, with a documented workaround.
**Status:** open, not started by decision.

## Context

Under `pool.executor = fiber`, socket operations suspend the fiber because the
`tcp` and `unix` stream transports are intercepted
(`sapi/fpmng/fpm/fpm_pool_fiber_xport.c`). `curl` does not go through those
transports — it has its own connection handling and its own event loop — so an
ext/curl request blocks the whole worker process and every other request in
flight with it.

The documented workaround (`docs/fiber_async_io.md`) is to use the PHP stream
layer instead: Guzzle's `StreamHandler` or Symfony's `NativeHttpClient`. Both go
through the intercepted transports and therefore become concurrent. The same
document records the caveat that this only pays off after the TLS handshake —
which is task 005.

The decision at the time was explicit: skip curl for now, because the workaround
covers the common case at zero cost.

## Problem

Revisit only if the workaround proves insufficient. Two things would change the
calculation:

- a library that cannot be moved off curl (some SDKs hard-depend on it)
- HTTP/2, which the stream layer does not provide and curl does

## Acceptance criteria (if it proceeds)

1. N concurrent requests each performing a curl request to a slow endpoint
   complete in roughly the time of one, measured.
2. Each request receives its own response body — asserted on data, not timing.
3. `pool.executor = classic` unaffected.
4. Anything that cannot be made non-blocking is refused with a clear message
   rather than silently blocking, following the precedent of the persistent
   connection refusal.

## Notes

- `curl_multi` plus a socket callback is the obvious direction, since it is
  designed to be driven by an external event loop. The awkward part is that
  userland calls `curl_exec()`, which is synchronous by contract — the semantics
  have to be preserved while the fiber suspends underneath.
- Before starting, check whether True Async upstream solves this for the `async`
  executor. If it does, doing it ourselves for `fiber` only may be the wrong
  investment.
