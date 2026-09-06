# php-fpm-ng

POC. PHP-FPM with additional operating modes: HTTP, supervisor, cron and
metrics — so that the container image holds one binary and the application
code, without nginx, without supervisord and without a system cron.

**Target audience: small projects.** One VPS, one instance, typically an
application plus one or two consumers plus a few cron jobs. Not k8s.

The argument is not performance — measured a few percent CPU under real load
(details in the notes). The argument is one configuration file describing the
whole thing and one binary to scan.

## State as of today

The HTTP gateway POC on libevent works, as a branch in php-src:
https://github.com/s2x/php-src/tree/fpm-http-poc

Verified 2026-09-05:

- full static build on musl (`-static-pie`) with opcache, mbstring, curl
  + OpenSSL, zlib, pdo_mysql, sockets, pcntl, posix
- runs in a bare `FROM scratch`, FPM as PID 1, HTTP 200, whole image
  20 MB in the minimal variant
- the frontend selects `pool.type = fastcgi | fastcgi-ng | http`; no directive
  means classic `fastcgi` and stays compatible with upstream FPM
- `fastcgi-ng` and `http` accept an optional
  `pool.executor = classic | fiber | async` (default `classic`; `async` is
  currently rejected during configuration validation)
- metrics: `pool.type = status` exposes a built-in `/metrics` (Prometheus)
  and `/status` (JSON); application metrics from PHP (`fpm_metric_register/inc/
  set/observe`, NOTES 3k/3w) through the `ext/fpmng_metrics/` extension,
  also from CLI via `fpm_metric_render()`
- the `fiber` executor is experimental and not intended for production, while
  `async` is currently rejected during configuration validation until it has
  the same hardening; limitations are described in `docs/async_errors.md` and
  `docs/NOTES.md`, sections 3t-3u

## Plan

Eventually a **separate SAPI** in `sapi/fpmng/`, not a fork of php-src —
`configure.ac` finds directories under `sapi/` by glob, so no existing file
needs to be touched. Details, decisions, measured numbers and the list of
known issues: [`docs/NOTES.md`](docs/NOTES.md).

## Recommended pool configuration for lightweight endpoints

Gain with no line of code, measured on the test box (details: `docs/NOTES.md`, 3m and 3t):

```ini
listen = /run/php/pool.sock          ; UDS instead of TCP loopback: -7..-11 us/req
php_admin_value[max_execution_time] = 0   ; no setitimer per request: -3 us/req
catch_workers_output = no            ; logs via error_log()/stderr to our own stack
request_cpu_tracking = no            ; if nobody reads "last request cpu" or %C
```

`request_terminate_timeout` still guards wall-clock time, so
`max_execution_time = 0` does not leave a request without a guard.

## Building

Scripts in `build/` run in an Alpine container, building out-of-tree:

```sh
docker run --rm \
  -v /path/to/php-src:/src \
  -v $PWD/build-dir:/build \
  -v $PWD:/out \
  alpine:latest sh /out/build/static-full.sh
```

Two flags without which this looks broken for no reason:

- `LDFLAGS=-static-pie` — plain `-static` does not work, because the Alpine
  toolchain defaults to PIE and the linker silently produces a dynamic
  binary, and the build still succeeds
- `PKG_CONFIG="pkg-config --static"` — otherwise static curl fails the
  configure test, because its dependencies are missing from the link line

Alpine has no `oniguruma-static`, so mbstring is built with `--disable-mbregex`.

## License

PHP License 3.01 — code comes from PHP-FPM.
