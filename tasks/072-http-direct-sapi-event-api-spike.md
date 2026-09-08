# 072 — Spike: SAPI event API and a shared scheduler extension (ext/fpm)

Status: open
Type: spike (decision-support prototype, not a feature commitment)

## Why

HTTP-direct workers each own a private libevent loop that no PHP code can
reach: the event base is internal to `fpm_http_direct.c`. Two capability
layers become possible if the SAPI exposes its loop and workers can wake each
other:

1. **SAPI event API** — C symbols (e.g. `fpm_event_add_fd` / timer
   registration) that a PHP extension can call during RINIT/request to attach
   callbacks to the worker's existing loop. This is how an extension "smuggles"
   scheduling logic into the server without a second loop.
2. **Cross-worker coordination** — shared-memory state plus an eventfd/pipe per
   worker so process B can wake process A's loop. Combined with layer 1 this
   yields a shared scheduler without touching PHP execution semantics:
   fair-accept steering (task 055), global connection limits (task 063),
   singleflight wakeups (task 069), and `fpm_push()` to connections owned by
   other workers (task 070).

The third layer — scheduling *execution* (suspending/moving running PHP) —
requires fibers and is explicitly out of scope here; this spike builds only
the I/O and wakeup layers, which work with the classic blocking executor.

Precedent: this is the architectural model Swoole uses (server loop exposed to
an extension with coroutines added later); the spike validates it on our SAPI.

## Questions to answer

1. What is the minimal SAPI surface (function set, ownership rules, lifecycle
   — what happens to registered events at request shutdown and worker
   recycling) that is safe to expose and stable enough to document?
2. Wakeup mechanism: eventfd/pipe per worker measured for latency and overhead
   under load; behavior when a target worker is blocked in classic PHP
   (wakeup deferred — quantify how bad, ties into task 055).
3. Shared-memory lifecycle: who creates/cleans state across reloads, and what
   is the recovery story when a worker dies holding state (epoch/lease)?
4. Prototype value: does a minimal `ext/fpm` prototype (timer + cross-worker
   wakeup + push-to-connection) demonstrably enable one concrete scenario, e.g.
   steering accepts or a cross-worker notification, on the poligon?

## Acceptance criteria

- A working prototype patch (not merged) demonstrating layers 1 and 2 with at
  least one end-to-end scenario, measured on the poligon: wakeup latency,
  accept-steering or notification effect, and CPU overhead when idle.
- Written answers to questions 1-3 with a proposed stable API sketch and the
  failure/recovery semantics, suitable as input to real feature tasks for
  055/063/069/070.
- A verdict on risk: what an extension bug can do to workers, and whether an
  opt-in build flag or runtime guard is required.
- Raw artifacts and prototype code retained outside the tree; negative result
  acceptable and recorded.

## Out of scope

- Fiber/async execution (separate concern; compatibility is assessed in task
  070, expected non-conflicting).
- Merging the SAPI event API or the extension; follow-up feature tasks are
  separate.
- Changing the `http` or `fastcgi` pool types.
