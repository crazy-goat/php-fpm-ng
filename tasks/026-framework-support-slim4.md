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

## Detailed test matrix

Nothing here is measured. The point of the matrix is that Slim is the **control
group**: if our diagnosis is right — that the trouble comes from process-wide
state, and specifically from parking a container in a class static — then most
of these should pass with no configuration beyond `FPMNG_SHARED_INCLUDES=1`.

Record a result for every row even when it passes trivially. A row that passes
here and fails in Laravel is evidence about *the framework*, which is exactly
what makes this worth running.

### Core, mirroring the other two frameworks

| Scenario | Assertion |
|---|---|
| `/mix`, N=8 | own MySQL row, own Redis value per request |
| `/session`, N=8 | own sid, own session data, `count` increments on round 2 |
| authenticated route, N=8 | own identity per request |
| object identity in every response | container and request instances distinct per request |

### Slim-specific

| Scenario | Risk | Assertion |
|---|---|---|
| PSR-7 request body stream | this project's entire concurrency story is about stream behaviour; a PSR-7 implementation may buffer, seek or lazily read the body | A's POST body is never visible to B; a large body is read completely |
| PSR-7 response body stream | `StreamInterface` writes reach output buffering, which we swap per request | A's output never appears in B's response |
| middleware stack | built per request or once? | middleware state does not carry between requests |
| container: built-in vs PHP-DI | PHP-DI compiles and caches; the question is whether any instance is static | no cross-request instance sharing |
| route cache enabled | cache file written once, shared per process | routing is correct; the cache is not rewritten per request |
| error middleware | our error handlers are swapped per request | A's error response does not appear in B's |

### The question this task exists to answer

| Question | Why it matters |
|---|---|
| does it need `fiber.isolate_statics` at all? | a **no** confirms the diagnosis; a **yes** means the problem is broader than two frameworks and every task above needs revisiting |
| does its stock `public/index.php` survive shared includes? | if yes, Slim is the first framework for which task 007 does not bite, and that is a documentable selling point |
| does anything fail that passes in Symfony? | would mean our model of the failure is incomplete |

### Versions

Pin and record: the Slim 4 minor version, **and** the PSR-7 implementation
(`slim/psr7`, `nyholm/psr7`, `laminas-diactoros`). They differ in stream
handling, which is the part most likely to interact with
`sapi/fpmng/fpm/fpm_pool_fiber_xport.c`. A result without both versions recorded
is not reproducible.
