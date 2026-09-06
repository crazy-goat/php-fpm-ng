# 025 — Laravel on the fiber executor: the statics list is the open risk

**Priority:** high. It works, and the way it works carries a specific danger
that needs to be either bounded or documented very plainly.
**Status:** open.

## Where it actually stands

Measured on Laravel 13.30.1 (`SESSION_DRIVER=redis`, `CACHE_STORE=redis`,
`REDIS_CLIENT=phpredis`, Eloquent on MySQL), `pool.executor = fiber`,
`env[FPMNG_SHARED_INCLUDES] = 1`, `pm.max_children = 1`.

With this configuration:

    fiber.isolate_statics = Illuminate\Container\Container::instance,\
                            Illuminate\Support\Facades\Facade::app,\
                            Illuminate\Support\Facades\Facade::resolvedInstance,\
                            Illuminate\Database\Eloquent\Model::resolver

- `/session`, 8 concurrent: 8/8 correct, distinct `app_oid` and
  `container_request_oid`. The current runner's empty-list negative control
  returns HTTP 200 responses that all share one `app_oid` and fails its data
  assertion; the earlier hand run measured 4-5/8 wrong.
- `Auth::login` for two users followed by 8 concurrent `/me`: 8/8 correct.
- The repository-owned Eloquent probe found that `Model::$resolver` also needs
  isolation: with the four-entry list, concurrent Eloquent and mixed DB/Cache/
  Redis requests are 8/8 correct; with only the previous three entries they
  return HTTP 500 with `Cannot execute queries while other unbuffered queries
  are active`.

Application changes still required, in `public/index.php`:

1. `require` instead of `require_once` for `bootstrap/app.php`
2. `defined('LARAVEL_START') || define(...)`, because constants are process-wide

(The third change previously needed — touching `$_SERVER`, `$_ENV`, `$_REQUEST`
to force the autoglobals — is no longer necessary.)

## The open risk, stated plainly

**The list of statics is empirical, and the failure mode is silent.**

The spike concluded that `Container::$instance` alone was sufficient. It was —
for `/session`. Adding one more scenario, an authenticated flow, immediately
required two more entries: without them most concurrent `/me` requests returned
`user: null`, because the `AuthManager` resolved through the Facade cache
belonged to whichever request had bootstrapped last. The repository-owned
Eloquent scenario then found a fourth required entry, `Model::$resolver`; with
only the three-entry list, Eloquent reused a PDO connection that still had
another request's unbuffered query active and returned HTTP 500.

Nothing tells us the list is complete. A code path nobody has exercised may need
a fifth entry, and when it does, Laravel may return HTTP 200 with another user's
data and log nothing.

## What this task must produce

1. **A systematic answer instead of an empirical one.** Options, in rough order
   of ambition:
   - enumerate the request-scoped statics in `vendor/laravel/framework` properly
     (the spike counted 237 `static $` declarations and hand-picked 12; only 3
     were ever confirmed) and justify a list
   - detect the problem at runtime rather than relying on a list — e.g. notice
     when an isolated-looking static changes identity across a suspension point
   - decide the list cannot be made reliable, and say so
2. **A published, versioned configuration snippet** for Laravel, with the
   Laravel version it was verified against. Not a suggestion in prose.
3. **Tests** in the framework harness from task 027, asserting on data. At minimum the two
   scenarios already measured, plus queues, broadcasting and any other subsystem
   that keeps a resolved instance in a static.
4. **A user-facing warning** wherever this configuration is documented, saying
   what happens when the list is incomplete: correct-looking responses with
   another user's data. Anyone deploying this must know that before, not after.

## Explicitly out of scope

- Octane comparison. Octane never has two requests in flight in one worker, so
  it does not face this problem; noting that is enough.
- Modifying Laravel.

## Notes

- The mechanism itself (`sapi/fpmng/fpm/fpm_pool_coop_statics.c`) knows nothing
  about Laravel and should stay that way. Everything in this task is about the
  configuration and its verification, not about the C code.

## Detailed test matrix

Everything below is **unmeasured** unless stated. Laravel's specific danger is
that state lives in class statics, so the question for almost every item is the
same: *does this subsystem park a resolved instance in a static, and is that
static on the isolation list?*

### Already measured (turn these into tests first)

| Scenario | Assertion | Status |
|---|---|---|
| `/session`, N=8 | own sid, own `sess_user`, distinct `app_oid` / `container_request_oid` | 8/8 with the four-entry list |
| same run, `fiber.isolate_statics` empty | must reproduce the failure | `ERROR`: all responses shared one `app_oid`; negative control holds |
| `Auth::login` + N=8 `/me` | own identity per request | 8/8 with the four-entry list; empty-list control fails |
| DB + Cache + Redis + Eloquent, N=8 | own values and no shared unbuffered PDO query | 8/8 with `Model::$resolver`; `ERROR` with only three entries |
| facade/object identity, N=8 | distinct application, request, container and facade roots | 8/8 |
| middleware + `terminate()`, N=8 | own request marker in response and termination record | 8/8 |
| CSRF and validation flash data, N=8 | own token/error data; cross-session token rejected | 8/8 |
| sync queue and broadcast, N=8 | dispatching request's context and own event marker | 8/8 |

### Facades — each one is a candidate for the isolation list

The list currently has four entries. It was extended for the auth flow and
then for Eloquent's `Model::$resolver`. The runner covers DB, Cache, Redis,
Session, Event, Queue and facade application identity; Log, Config, View, Route
and Mail remain unmeasured or only indirectly exercised. Every facade below
resolves through the same cache and needs a concurrency test before we can claim
the list is complete.

| Facade | Assertion under N=8 concurrency |
|---|---|
| `DB::` | each request's query results are its own; no `Cannot execute queries while other unbuffered queries are active` |
| `Cache::` | a value written by A is not read by B under a different key |
| `Redis::` | replies match requests; no `unserialize` of another request's response |
| `Session::` | covered by `/session`, but assert the facade path specifically |
| `Log::` | Monolog handlers hold open file handles; lines are not interleaved mid-line or attributed to the wrong request |
| `Config::` | config is boot-once and shared by design — confirm nothing writes to it per request |
| `View::` / Blade | view composers and shared data resolved per request |
| `Event::` | listeners registered during boot; a listener registered by A does not fire for B |
| `Queue::` (sync driver) | the job runs in the dispatching request's context |
| `Route::` | route model binding resolves the caller's model |
| `Mail::` (array/log transport) | messages are attributed to the right request |

### Eloquent

| Scenario | Risk | Assertion | Status |
|---|---|---|---|
| `Model::$resolver` | a static holding the connection resolver | A and B use their own connection | 8/8 with the fourth isolation entry; HTTP 500 with only three |
| model events / observers | registered statically during boot | an observer registered by A does not fire for B's model | NOT MEASURED |
| global scopes | stored statically on the model class | a scope applied by A does not leak into B's query | NOT MEASURED |
| `Model::$booted` | boot-once **per process** here, not per request as under classic FPM — a known side effect of the coop model, consequence unexplored | boot side effects are not request-dependent | NOT MEASURED |

### Request lifecycle

| Scenario | Risk | Assertion | Status |
|---|---|---|---|
| CSRF middleware | token in the session | A's token validates only A's request | 8/8; cross-session token rejected |
| rate limiter | cache-backed, keyed by identity | limits are attributed to the right user | NOT MEASURED |
| validation with a redirect back | errors flashed into the session | A's errors never render in B's response | 8/8 |
| middleware groups / `terminate()` | terminable middleware runs after the response | attributed to the right request | 8/8 |
| `LARAVEL_START` | constants are process-wide and will stay that way | first request's value survives; nothing depends on it being per request | observed defined; value isolation NOT MEASURED |

### Configuration completeness — the point of this task

| Scenario | Assertion |
|---|---|
| every scenario above, with `fiber.isolate_statics` **empty** | the ones that need isolation must fail; this is what proves the list is doing the work |
| a scenario that fails only with an incomplete list | document it — this is the evidence for how the failure presents |
| the final list, with the Laravel version it was verified against | published in `docs/frameworks.md` as a versioned snippet |

### Versions

Only Laravel **13.30.1** has been tested. The repository fixture locks
`predis/predis` **3.6.0**; the measured test-box run used phpredis **6.3.0RC1**.
A framework upgrade may add or move a static; the list must be re-verified per
minor version, and that requirement belongs in the user-facing documentation.
