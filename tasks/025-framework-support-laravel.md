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
                            Illuminate\Support\Facades\Facade::resolvedInstance

- `/session`, 8 concurrent: 8/8 correct, distinct `app_oid` and
  `container_request_oid`. A negative control with the directive empty
  reproduced the original 4-5/8 failure.
- `Auth::login` for two users followed by 8 concurrent `/me`: 8/8 correct.

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
belonged to whichever request had bootstrapped last.

Nothing tells us the list is complete. A code path nobody has exercised may need
a fourth entry, and when it does, Laravel will return HTTP 200 with another
user's data and log nothing.

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
