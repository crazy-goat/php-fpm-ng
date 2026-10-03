# Frameworks on `main`

`main` ships the `classic` executor (one request at a time per worker, like
upstream FPM) and the beta `worker` executor on `pool.type = http-direct`, which
holds several requests per worker. Under `classic` a framework runs as it does under upstream PHP-FPM: every request
starts from a clean request state, so nothing in this repository needs framework
specific configuration.

What is **not** claimed here: no framework (Symfony, Laravel, Slim 4, ...) is
exercised by an automated test on `main` yet. Making the harness in
`tests/frameworks/` run on `main`'s executors is tracked in issue #602.

## Fiber and async executors

The `fiber` and `async` executors run several requests concurrently in one
worker process, which only helps a framework that keeps no state outside what is
isolated per request. They were removed from `main` (issue #373) and live on
branch `async`, together with their framework measurements, the required
configuration (`env[FPMNG_SHARED_INCLUDES]`, `fiber.isolate_statics`) and the
Symfony, Laravel and Slim 4 verdicts:
`async/docs/frameworks-fiber.md` on branch `async`.
