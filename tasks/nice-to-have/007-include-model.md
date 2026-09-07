# 007 — Decide and implement the include model for shared includes

**Track:** nice-to-have. This task only applies to `pool.executor = fiber`,
which is moving behind a build flag that is **off by default**, so a stock
binary does not contain this code at all. The `Priority:` line below is the
priority *within* the fiber track — it is not a claim against the HTTP,
cron, scheduler or proxy work, which is where the project is focused.

**Priority:** medium. Currently resolved by an undocumented default, which is
the worst of the options.
**Status:** open. This is item 4 of the fix list in `docs/frameworks.md`.

## Context

Under `pool.executor = fiber` with `FPMNG_SHARED_INCLUDES=1`, `EG(included_files)`
is shared across requests in the process. That removes the "Cannot redeclare
class ComposerAutoloaderInit..." wall that otherwise kills the second request of
any Composer application (measured: 30/30 requests succeed with it, request 2
fatals without it — `docs/fiber_errors.md`, section "EXPERIMENT").

It costs three things, documented in the same section:

1. `require_once` on an already-included file returns `true` instead of the
   file's return value
2. code at a file's top level runs only once per process, silently
3. deploying new code needs a `SIGUSR2`, since files are not re-read

Cost 1 was originally going to be fixed with a "remember the value `require_once`
returned" cache. **That was measured and rejected**: it fixes Laravel's
`$app = require_once bootstrap/app.php` but does nothing for Symfony, whose
entire runtime is a *side effect* of the include. When the include becomes a
no-op, Symfony returns HTTP 200 with an empty body and nothing in the log
(`docs/frameworks.md`, "Why caching `require_once` values will NOT help").

Today the project's answer is "write your own `index.php`" — but that is the
answer by omission, not by decision. It is written in `docs/frameworks.md` as a
measurement note, not as a requirement anyone would find before hitting it.

## Problem

Choose the include model deliberately, and make the choice visible to users.

The two candidates on the table:

- **A. Document the requirement.** Shared includes require an entry script that
  contains no declarations and whose meaning does not depend on `require_once`
  return values or on top-level code running per request. Symfony needs a
  hand-written `public/index.php` (seven lines, no `symfony/runtime`); Laravel
  needs three changes to its own. Both are in `docs/frameworks.md`.
- **B. Per-request `included_files` with redeclaration skipping.** Keep the file
  list per request so `require_once` behaves normally and top-level code runs
  each time, and make recompilation skip re-declaring classes and functions that
  already exist in the process tables. This is the honest fix and the expensive
  one.

## Acceptance criteria

Whichever option is chosen:

1. The decision is written in `docs/` with its reasoning, and referenced from
   `README.md` where the fiber executor is described. A user must be able to
   learn the constraint **before** their second request fatals.
2. The chosen behaviour is verified against **both** Symfony and Laravel, with
   raw output, in the same shape as the existing measurements.
3. If option A: the failure mode for a non-conforming entry script is
   diagnosable. Today it is `Cannot redeclare class ComposerAutoloaderInit<hash>`
   pointing into `vendor/composer/autoload_real.php:5`, which does not tell
   anybody what to change. A message naming the actual requirement is part of
   the task.
4. If option B: `require_once` returns the file's value, top-level code runs per
   request, and no redeclaration error occurs — all three demonstrated, plus the
   memory and latency cost of recompilation measured against the current
   shared-includes numbers (Symfony: 172 req/s, RSS 40.2 → 42.4 MB over 600
   requests).

## Explicitly out of scope

- The `require_once` **value cache** on its own. It is measured, it does not
  solve the problem, and it should not be revisited without new evidence.
- The `SIGUSR2`-on-deploy consequence — tracked separately (see the task on
  symlink deploys and `fiber.revalidate_freq`).

## Notes

- Option B interacts with opcache and with `fpm_pool_coop_reval.c`, which hooks
  `zend_compile_file` behind opcache to notice changed files. Anyone attempting
  B should read that file first.
