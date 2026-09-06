# 026 — Slim 4 on the fiber executor: unknown, and the cheapest one to find out

**Priority:** medium. Nothing is known yet, and it is the framework most likely
to work with no special configuration at all.
**Status:** open. **Nothing has been measured. Everything below is expectation,
not evidence.**

## Why it is worth testing

Symfony and Laravel are the two heavy cases and both needed work. Slim 4 sits at
the other end: a micro-framework, PSR-7 and PSR-15, a container that is normally
passed explicitly rather than parked in a class static, and no runtime component
rewriting the entry script.

If the pattern established so far holds — that the trouble comes from
process-wide state, and specifically from frameworks keeping a container in a
class static — Slim ought to need nothing beyond `FPMNG_SHARED_INCLUDES=1`.

That expectation is exactly why it is worth measuring: it is the cheapest
available test of whether we understand the failure mode or have merely patched
two specific frameworks.

## What to measure

Build a probe application mirroring the Symfony and Laravel ones already on the
test box (`~/rd/apps/symfony`, `~/rd/apps/laravel`), so results are comparable:

- `/mix` — MySQL and Redis in one request, with a `sleep` parameter, returning
  per-request identifiers
- `/session` — PHP sessions, returning session id and a counter
- `/me` — something authenticated, if Slim's usual middleware makes that
  reasonable
- object identity in every response (container, request), which is what exposed
  the Laravel leak

Then run what the others ran: N=8 concurrent with `pm.max_children = 1`,
asserting on **data**, not HTTP status.

## Acceptance criteria

1. A verdict in `docs/frameworks.md` in the same shape as the existing two:
   yes / no / conditional, with the conditions and the raw numbers.
2. An explicit answer to: does it need `fiber.isolate_statics`, and if so, for
   what? A "no" here is a meaningful result and confirms the diagnosis; a "yes"
   is more interesting still, because it would mean the problem is broader than
   two frameworks.
3. An explicit answer to: does it need a hand-written entry script, or does its
   normal `public/index.php` survive shared includes? Slim's entry script is
   usually plain — if it works unmodified, that is the first framework for which
   task 007 does not bite.
4. Whatever is not measured is written down as not measured.

## Explicitly out of scope

- Adding Slim-specific support in C. If Slim needs something the mechanism
  cannot express, that is a finding, not a licence to special-case it.

## Notes

- Pick and record a specific Slim 4 version and a specific PSR-7 implementation
  (slim/psr7, nyholm/psr7, laminas-diactoros). PSR-7 implementations differ in
  how they handle streams, and this project's whole concurrency story is about
  stream behaviour.
- Worth checking whether the PSR-7 request body stream interacts with the
  transport interception in `fpm_pool_fiber_xport.c`.
