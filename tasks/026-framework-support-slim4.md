# 026 — Slim 4 on the fiber executor: unknown, and the cheapest one to find out

**Priority:** medium. Slim 4 is the framework most likely to work with no special
configuration at all; this task records the first repository-owned measurement.
**Status:** in progress. The repository probe has measured the core and all
listed Slim-specific scenarios below on the current-main fiber build. Broader
controls and integrations remain explicitly open; the result is evidence, not an
expectation.

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

The matrix below defines the coverage. The measured result is recorded in the
Outcome section at the end of this task. Slim is the **control group**: if our
diagnosis is right — that the trouble comes from process-wide
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

## Outcome so far — 2026-09-06

The repository probe under `tests/frameworks/slim4/` ran against the current-main
fiber build with Slim `4.15.3`, `slim/psr7` `1.8.0`, and `predis/predis` `3.6.0`.
With `pool.executor = fiber`, `FPMNG_SHARED_INCLUDES=1`, `pm.max_children = 1`,
MySQL database `slim4`, and Redis database `2`, the final combined run
(`SLIM_CONTAINER=php-di SLIM_ROUTE_CACHE=1`) reported **11 PASS, 0 ERROR, and
0 NOT MEASURED**. The passing rows cover the shared-includes entry script,
`/mix`, two session rounds, the authenticated route, object identity, request
and response PSR-7 streams, middleware state, error middleware, the PHP-DI
container variant, and Slim route-cache mode.

This is a conditional **YES** for the measured Slim 4 probe: the stock
`public/index.php` survives repeated shared-includes requests, and no
`fiber.isolate_statics` entries are needed by the tested Slim application. The
runner uses the HTTP gateway's bare front-controller routes; no Slim-specific C
support was added.

The PHP-DI container variant and route-cache mode are **PASS**: with
`SLIM_CONTAINER=php-di` eight concurrent requests each used a distinct PHP-DI
container and a distinct container service (`php-di/php-di` `7.1.1`), and with
`SLIM_ROUTE_CACHE=1` cached routing stayed correct under concurrency while the
`$app->getRouteCollector()->setCacheFile()` cache file's fingerprint (device,
inode, size, mtime, content hash) was unchanged before and after the run. Also
not measured are negative-control/classic comparisons, other Slim or PSR-7
versions, `pm.max_children > 1`, `fiber.revalidate_freq`, and broader framework
features outside this probe. Task 026 remains in progress until the unmeasured
matrix rows are either run or intentionally closed.
