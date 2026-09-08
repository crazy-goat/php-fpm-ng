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

## Amendment 2026-09-08 — design the API against Revolt, and do not write a driver in C

Source: `docs/http-direct-revolt-integration.md` (revision 2026-09-08) and the
POC in task 073.

1. **The event API is the deliverable; the driver is userland.** Revolt's driver
   contract is four abstract methods plus two overrides
   (revolt/event-loop v1.0.9,
   `src/EventLoop/Internal/AbstractDriver.php:399-444`), and a driver over
   libevent already exists as a ~200-line PHP file (`Driver/EventDriver.php`,
   backed by `ext-event`). So layer 1 of this spike must expose libevent
   primitives — create/enable/disable/free an fd, timer or signal watcher, and
   pump the loop once with and without blocking — and nothing Revolt-shaped in
   C. Question 1 ("minimal SAPI surface") is answered by whichever primitive set
   makes an unmodified `revolt/event-loop` run; that is a checkable target
   rather than a matter of taste, and it keeps library-version risk out of the
   binary.
2. **Reentrancy is a hard constraint, not a performance note.** A driver cannot
   pump the same event base from inside a callback of that base: measured on the
   test box, libevent 2.1.12-stable returns -1 and warns
   `event_base_loop: reentrant invocation`. Since PHP currently executes inside
   the evhttp callback (`sapi/fpmng/fpm/fpm_http_direct.c:508`, `:518`,
   `:412`, `:431`), any request whose PHP calls `await` would fail. Therefore
   the spike must state which of the two ownership models it targets — the SAPI
   pumping the loop with PHP as a callback (classic direct: no userland `await`
   possible, watchers progress only between requests), or PHP pumping the loop
   (worker mode, task 073). They are different products, and the API must say
   which one it serves.
3. **The name `ext/fpm` is not required.** Task 073 registers its functions from
   the pool type's own child, so the POC needs no extension at all. Whether the
   stable form is a bundled extension or SAPI-registered functions is now an
   open question for this spike rather than a premise of it.

Layer 2 (cross-worker wakeup, shared memory) is unaffected by this amendment.
