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

## Automated probe

`tests/frameworks/symfony/run.sh` now provisions a fresh copy of the pinned
application, a private MySQL database and Redis namespace, and runs the
concurrency assertions. It was measured on 2026-09-06 against the verified
php-fpm-ng binary `PHP 8.5.11-dev` with SHA-256
`f9eeb62304b4fe609f3180e44c493f941c12f7a675d433826b7b4f7861b356b6`. The
application used Symfony 8.1.6, Doctrine ORM 3.6.8, DBAL 4.4.4, and phpredis
loaded as the Redis client. The source checkout recorded by the runner was
`65ec755`.

The measured report was `PASS=6 ERROR=0 NOT MEASURED=4`:

- `/mix`, sessions including the second cookie round, stateful `http_basic`
  including the no-header round, and object identity all passed with eight
  concurrent requests;
- the missing `FPMNG_SHARED_INCLUDES` control reproduced the request-two
  Composer redeclaration in the response body (HTTP status was 200), and the
  unsupported `fiber.isolate_statics`/`async` configuration control was
  rejected;
- `APP_ENV=prod`, `pm.max_children > 1`, `fiber.revalidate_freq`, and the
  additional framework surface/long RSS run remain explicitly `NOT MEASURED`.

The task remains open: this first automated probe does not yet cover every
matrix item or provide the user-facing support statement requested above.

## UPDATE 2026-09-06 (second session): matrix items measured

The probe now covers the remaining matrix items. Measured on 2026-09-06
against a locally built php-fpm-ng `PHP 8.6.0-dev (fpm-fcgi) (built:
Sep  6 2026 15:00:30) (NTS)`, SHA-256
`862c180037fee77b38c64ee50c359a8d257b336c4fbea13266a9c9a0d6e3a881`, all four
feature markers verified via `strings`, source commit `65ec755` (worktree
branch `task/024-symfony-tests`), Symfony 8.1.6 with predis.

Full-run result: **PASS=19 ERROR=2 NOT MEASURED=0** over 21 scenarios.

- `APP_ENV=prod` — the four core scenarios pass on a dedicated prod pool
  (`APP_DEBUG=0`); each response reports `environment: prod`. **Measured.**
- `pm.max_children = 2` — mix and object-identity pass; **session and
  stateful-auth intermittently stall** in the cookie-replay round (some
  requests never reach the scenario gate, completing exactly at the 90 s
  BLPOP timeout; `llen` observed 2-7 of 8). The failure reproduces in every
  full run in at least one pm2 scenario, across several runs, including after
  isolating each scenario on a fresh pool. **Retained as ERROR.**
- `pm-max-children` (derived scenario) is therefore recorded ERROR and the
  support claim for `pm.max_children > 1` with stateful Symfony traffic is
  **not supported** on this build.
- `fiber.revalidate_freq` — controlled deploy: the pool starts with
  `fiber.revalidate_freq = 1`, the probe changes `DeployMarker::VALUE` on
  disk, and subsequent requests serve the updated code without a manual
  restart. **PASS.**
- Longer run watching RSS — 200 sequential requests in one worker; RSS from
  `getrusage()` (portable: `/proc/self/status` on Linux), growth ≤ 6144 KiB
  (measured 1.6-1.7 MB). **PASS.**
- Framework surface — Twig `renderView` with a custom extension doing real
  blocking Redis I/O inside the template (`app.user` and request data
  asserted in the rendered HTML), form submit with a `NotBlank` constraint
  (valid/invalid split asserted on validator errors), and synchronous
  Messenger dispatch with the handler result asserted through `HandledStamp`.
  **All PASS** under 4-5 concurrent gated requests.
- Fixes made to the probe along the way: per-scenario fresh pools, per-suite
  unique Redis gate names, warm-up request before each gated window, portable
  RSS reporting (a failed `fopen('/proc/self/status')` inside the pool is
  fatal for the whole response on this build — suppressed or not), 90 s gate
  windows, per-pool logs (each pool start truncated the previous pool's log).

The task remains open for: root-causing the `pm.max_children = 2` stall, the
user-facing support statement in `README.md` (item 3), and the version-scope
decision (item 4).

## Detailed test matrix

Everything below is **unmeasured** unless this document says otherwise. The
"risk" column says why the item is on the list at all — an item with no
mechanism behind it does not belong here.

### Already measured (turn these into tests first)

| Scenario | Assertion | Status |
|---|---|---|
| `/session`, N=8 | own sid, own `sess_user`, `count` increments on round 2 | 8/8 |
| `http_basic` + session, N=8 | own identity with, and **without**, the `Authorization` header | 8/8 both rounds |
| `/mix`, N=8 | own MySQL row, own Redis value, own cache value | 8/8 |
| 600 sequential | no errors, RSS stable (40.2 → 42.4 MB) | passed |

### Session-backed state — same mechanism as the failures we already found

| Scenario | Risk | Assertion |
|---|---|---|
| CSRF token in a form | tokens live in the session; a shared session mixes them | request A's form validates only with A's token, and rejects B's |
| Flash messages | session-backed, read-once | a flash set by A is never visible to B |
| `logout()` under concurrency | invalidates the session and migrates the id | logging A out leaves B authenticated |
| remember-me cookie | separate token storage path from the session | A's cookie never authenticates B |
| two firewalls, different contexts | token storage keyed per firewall | identities do not cross between firewalls |

### Doctrine — per-request object graph on a shared connection

| Scenario | Risk | Assertion |
|---|---|---|
| identity map | the EM is per request, the connection is intercepted | A and B fetching the same row get their own entity instances |
| transaction across a suspension | one connection, two requests in flight | A's uncommitted write is invisible to B; interleaving does not commit A's work inside B's transaction |
| lazy proxy resolved after suspension | the proxy loads mid-request, after a fiber switch | the loaded data belongs to the request that owns the proxy |
| EM closed by an exception | `EntityManager::close()` — is it per request? | B's EM still works after A's is closed |

### Output and the response lifecycle

| Scenario | Risk | Assertion |
|---|---|---|
| `StreamedResponse` | output buffering is swapped per request (`OG`), but `flush()` writes to a real socket | A's chunks never appear inside B's response body |
| file upload | `$_FILES` and rfc1867 temporary-file handling are process-level | each request sees only its own upload; temp files are cleaned up per request |
| `RedirectResponse` and headers | `SG(sapi_headers)` is per request | headers do not leak between concurrent responses |
| exception → error page | our error handlers are swapped per request | A's exception page does not appear in B's response |
| `kernel.terminate` | runs after the response is sent, still inside the fiber | terminate work attributed to the right request |

### Framework surface never touched

| Scenario | Risk |
|---|---|
| Twig with `app.user` and globals | Twig caches the environment; globals resolved per request |
| translator / `setLocale` | locale is per request, catalogues are cached per process |
| Messenger `dispatch()` (sync transport) | handlers resolved from the container |
| cache pool invalidation by tag | tag store is shared; invalidation is global by design — confirm it is not *accidentally* per request |
| Serializer / normalizer with circular refs | keeps state during a single normalization |
| `APP_ENV=prod` | the entire measured history is `dev`; prod is a different code path (compiled container, no profiler) |
| `pm.max_children > 1` | never run; the interaction of several workers with shared includes is unverified |

### Versions

Only Symfony **8.1.6** has been tested. Record the version with every result.
The `symfony/runtime` interaction that forces a hand-written `index.php`
(task 007) is version-sensitive and must be re-checked per major version.
