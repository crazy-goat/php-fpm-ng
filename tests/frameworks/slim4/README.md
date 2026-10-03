# Slim 4 smoke test

The stock Slim 4 `public/index.php` (`require ../vendor/autoload.php`, nothing framework specific
in `sapi/fpmng/`) answers through the executors `main` ships. Issue #602.

## Versions

Slim `4.15.3`, `slim/psr7` `1.8.0`, PHP-DI `7.1.1`, pinned in `composer.json` and `composer.lock`.

## What `bin/run.sh` does

For each setup it starts a static pool, waits for `/health`, runs `bin/run.php` and stops the pool.
`http-direct` has one worker, so every request lands in the same process. The `fastcgi` pool behind the
gateway has 8 (one per parallel probe request) and the gateway runs as one process (`http.gateways = 1`):
with 2 gateway processes some of a parallel burst stayed queued until the wait timeout and got 503.

- `gateway-fastcgi`: `pool.type = gateway` in front of `pool.type = fastcgi`;
- `http-direct`: `pool.type = http-direct`, `pool.executor = classic`.

Each setup runs plain and with `SLIM_CONTAINER=php-di SLIM_ROUTE_CACHE=1`. Set either variable to
run only that combination; set `MODES` to pick setups.

`bin/run.php` reports every scenario as `PASS`, `ERROR` or `NOT MEASURED` and exits 1 on any
`ERROR`:

- `entry-script-shared-includes`: repeated requests through the stock entry script;
- `request-state-reset`: 24 sequential requests; none sees a PHP global, a session user or a
  middleware id of an earlier one;
- `session-rounds`, `authenticated-route`: 8 parallel cookie jars keep their own session; a
  request without a cookie sees no login;
- `psr7-request-body`, `psr7-response-body`, `error-middleware`: 8 parallel requests keep their
  own 64 KiB body, response and exception;
- `container-php-di`, `route-cache`: only in their combination; the cache file is unchanged after
  requests.

Object ids are not compared: they are reused once a request frees its objects, so they prove
nothing under `classic`.

## Running

```sh
FPMNG=/path/to/php-fpm-ng PHP=/path/to/php ./bin/run.sh
```

Defaults: FastCGI port `22625`, HTTP port `22626`; `choose_free_port` bumps past a port that is
taken. Override with `FCGI_PORT`, `HTTP_PORT`, `RUN_DIR`. The script stops only the master recorded
in its own `RUN_DIR`.
