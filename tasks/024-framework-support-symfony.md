# 024 — Symfony on the fiber executor: from "measured once" to "supported"

**Priority:** high. Symfony is the closest thing this project has to a working
target, and the gap between that and "supported" is entirely test coverage.
**Status:** open.

## Where it actually stands

Measured on Symfony 8.1.6 (skeleton + orm-pack + security-bundle, Doctrine ORM
3.6, sessions and cache on Redis), `pool.executor = fiber`,
`env[FPMNG_SHARED_INCLUDES] = 1`, `pm.max_children = 1`. Full numbers in the
sections above in this document.

Working, concurrently, in one worker process:

- MySQL through DBAL and ORM, Redis as session store and cache pool —
  8 concurrent requests, each with its own data
- PHP sessions — 8/8 own session id, `count` incrementing on a second round
- the stateful `http_basic` firewall, including the round with **no**
  `Authorization` header where the token is restored from the session
- throughput: 172 req/s versus 15.5 req/s on `classic` over 600 requests, RSS
  40.2 → 42.4 MB, same worker, zero errors

Required configuration, and the reason each is required:

- `FPMNG_SHARED_INCLUDES=1` — without it request 2 fatals on
  `Cannot redeclare class ComposerAutoloaderInit<hash>`
- a hand-written `public/index.php` without `symfony/runtime` (seven lines) —
  see "Why a `require_once` value cache does NOT help" above; this is task 007
- **no** `fiber.isolate_statics` entries; Symfony keeps its state in the
  container and the session, both already per request

## What "supported" needs that we do not have

Everything above was measured **by hand, once, on one machine**. There is no
automated test, so any commit can silently undo it and we would find out by
accident.

1. The measurements above, as automated tests, in the framework harness from
   task 027. Assertions on **data**, not HTTP status —
   the Laravel failure this document records returned HTTP 200 for every request
   while serving other users' sessions.
2. Coverage of what was never measured at all:
   - `APP_ENV=prod` (everything so far ran in dev, which is the slow path and a
     different code path)
   - `pm.max_children > 1`
   - `fiber.revalidate_freq`, including the deploy story
   - a longer run than 600 requests, watching RSS
   - Twig rendering, the form component, validation, messenger — none of the
     framework surface beyond the four probe endpoints has been touched
3. A written, user-facing statement of what is supported and what is not, in
   `README.md` rather than only here. Someone deciding whether to use this needs
   the constraints before they start, not after.
4. A decision on which Symfony versions are in scope. Only 8.1.6 has been
   tested; the `symfony/runtime` interaction is version-sensitive.

## Explicitly out of scope

- Making the hand-written `index.php` unnecessary. That is task 007.
- Performance tuning. The numbers are already good enough to justify the work;
  correctness coverage is what is missing.

## Notes

- The probe application used for all of this lives on the test box in
  `~/rd/apps/symfony`, with its endpoints in `src/Controller/ProbeController.php`
  (`/mix`, `/sleep`, `/session`, `/me`, `/who`, `/leak`). It is worth turning
  into something the repository owns rather than something that exists only on
  one machine.
