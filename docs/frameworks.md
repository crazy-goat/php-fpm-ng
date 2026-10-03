# Frameworks on `main`

`main` ships the `classic` executor (one request at a time per worker, like
upstream FPM) and the beta `worker` executor on `pool.type = http-direct`, which
holds several requests per worker. Under `classic` a framework runs as it does under upstream PHP-FPM: every request
starts from a clean request state, so nothing in this repository needs framework
specific configuration.

## What is tested

One framework, Slim 4, is exercised by an automated test on `main`:
`tests/frameworks/slim4/` runs the stock `public/index.php` (no framework specific
configuration) through

- `pool.type = gateway` in front of `pool.type = fastcgi` (classic executor), and
- `pool.type = http-direct` with `pool.executor = classic`,

each plain and with PHP-DI plus Slim's route cache (http-direct has one worker, the fastcgi pool 8). CI runs it in the
`frameworks` job of `.github/workflows/build-matrix.yml` (issue #602). It checks that:

- requests started from a clean request state (no PHP global, middleware state or session user
  carried over from the previous request in the same worker);
- sessions, an authenticated route, 64 KiB POST bodies, response bodies and error responses
  stay correct under 8 parallel requests;
- the route cache file is not rewritten while requests are served.

What is **not** claimed: Symfony and Laravel are not tested on `main`, and no database or
cache backend is involved. Run it by hand with
`FPMNG=/path/to/php-fpm-ng tests/frameworks/slim4/bin/run.sh`
([`tests/frameworks/README.md`](../tests/frameworks/README.md)).

## Recipes

Symfony and Laravel configurations for the classic executors, and what was and was
not tried with them: [`guides/framework-recipes.md`](guides/framework-recipes.md).

## Fiber and async executors

The `fiber` and `async` executors run several requests concurrently in one
worker process, which only helps a framework that keeps no state outside what is
isolated per request. They were removed from `main` (issue #373) and live on
branch `async`, together with their framework measurements, the required
configuration (`env[FPMNG_SHARED_INCLUDES]`, `fiber.isolate_statics`) and the
Symfony, Laravel and Slim 4 verdicts:
`async/docs/frameworks-fiber.md` on branch `async`; the fiber probe harness (Symfony, Laravel,
Slim 4) is `async/tests/frameworks/` there.
