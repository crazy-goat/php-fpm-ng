# Experimental direct HTTP: classic PHP, static FPM pool

`pool.type = http-direct` runs libevent's HTTP parser and PHP in each FPM child.
It does **not** start gateway children, create an internal FastCGI listener, or
serialize requests/responses as FastCGI records. The master still creates,
monitors, replaces, and signals normal FPM children.

This is a **POC, not a production frontend**. Existing `http`, `fastcgi`, and
`fastcgi-ng` pools are unchanged.

```ini
[direct]
pool.type = http-direct
pool.executor = classic
listen = 127.0.0.1:8080
pm = static
pm.max_children = 4
pm.max_requests = 1000
chdir = /app/public
http.front_controller = /index.php
http.max_body = 1M
http.read_timeout = 5000
request_terminate_timeout = 30s
```

`pool.executor` may be omitted (classic). Other executors and `pm = dynamic` /
`ondemand` fail configuration validation. No fibers, persistent application
container, or shared request state: every request runs PHP startup, the script,
and PHP shutdown, including extension RINIT/RSHUTDOWN.

## Routing and PHP I/O

Every accepted path executes the configured front controller. Unlike the
`http` gateway, this is **not** a fallback after looking for a matching file.
The script is resolved inside `chdir`; client paths never select a script.
`SCRIPT_NAME` and `PHP_SELF` identify the configured front controller;
`REQUEST_URI` retains the raw URI, `QUERY_STRING` carries the query, and
`PATH_INFO` carries the parsed URL path. Headers map to `HTTP_*` (except Proxy,
which is not imported as `HTTP_PROXY`); content type/length have CGI-compatible
names. `getallheaders()` and `apache_request_headers()` read HTTP headers directly.

Bodies (including chunked requests) are buffered by libevent and supplied through
SAPI, supporting forms, raw `php://input`, and PHP's normal POST handling. PHP
supplies response status, headers (including repeated Set-Cookie), and body through
SAPI. The transport owns Content-Length/Transfer-Encoding and supports HEAD and
HTTP/1.x keep-alive.

## Deliberate limits

- Plain HTTP only. No static files, TLS, trusted-proxy handling, gateway ACLs,
  gateway access log, HTTP/2, WebSocket upgrades, or CONNECT/TRACE.
- Only `http.front_controller`, `http.max_body`, and `http.read_timeout` from the
  gateway's `http.*` directives are accepted. Other gateway options are rejected,
  even if explicitly set to an otherwise harmless default. `listen` is the HTTP
  endpoint; `http.listen` does not apply.
- `chdir` must be absolute; chroot and `listen.allowed_clients` are rejected.
  FPM ping/status listeners and the FastCGI access log are not implemented here;
  use a separate `pool.type = status` for the FPM scoreboard.
- The FastCGI-specific `.user.ini` / per-host/per-directory php.ini activation
  hook is not used. Use php.ini and the pool's `php_value` / `php_admin_value`.
- Requests have a 64 KiB header limit and a body limit of 32 MiB by default,
  configurable down to one byte. Body size cannot be zero/unlimited or above 32 MiB.
- Responses are buffered until PHP shutdown, capped at 8 MiB body and 64 KiB
  forwarded headers. Overflow produces HTTP 500 instead of a partial response.
  `flush()` freezes PHP headers but **does not stream to the client**.
  `fastcgi_finish_request()` is unavailable, not a misleading no-op.
- There are at most 16 pending PHP response writes per worker; further ready
  requests get 503. This is **not a total connection/memory bound**: partially
  read requests and idle connections also use memory.
- `http.read_timeout` must be positive. In this POC it is libevent's **inactivity**
  timeout for reads/writes, not the gateway's absolute whole-request deadline.
  PHP execution blocks this worker's event loop, including its timers. A trickling
  client can evade the inactivity timer; this is not slowloris protection.
- A worker can hold many HTTP connections but execute only one PHP request at a
  time. The shared listener can batch accepts unevenly, and keep-alive connections
  remain attached to their accepting worker. There is no gateway queue that can
  redistribute their requests to idle workers.
- A PHP crash or master-enforced timeout closes the worker's connections; there
  is no independent gateway left to generate HTTP 502/504. The master replaces
  the worker. SIGQUIT/reload drains already executing PHP and queued responses;
  idle/incomplete connections are not preserved across retirement.

`pm.max_requests`, request stage/idle/busy counters, and the master's execution
timeout are exercised by `sapi/fpmng/tests/fpmng-http-*.phpt`. These files
are automatically included by `build/run-fpmng-phpt.sh` and the CI `fpmng-phpt`
job. Production hardening, full framework compatibility, and a total memory bound
are **not measured/implemented**.

## Performance experiment

Run on a private directory and port range on the shared Linux test box:

```sh
python3 build/benchmark-http-direct.py /path/to/php-fpm-ng \
  /path/to/new-scratch-directory --base-port 28154
```

The harness verifies a distinctive binary string and SHA-256, checks response
bodies, then compares **nginx + FastCGI**, **HTTP gateway**, and **HTTP-direct**
using the same PHP binary and scripts. Four static PHP children per backend;
one nginx/gateway process; Unix-socket FastCGI upstreams (nginx's default
`fastcgi_keep_conn off`, gateway persistent upstreams); HTTP client keep-alive;
`wrk` at concurrency 1 and 32; three rotated 10-second runs after
2-second warmups. It retains raw output, throughput, p50/p99, server-process CPU
per request, and sampled end-of-run RSS. CPU/RSS include each backend's master,
PHP children, and nginx/gateway processes, but not the load generator.

Results will be recorded after the pre-PR poligon run. No performance claim is
made from architecture alone.

### Measured results (Intel i7-6700T, 8 threads, poligon, 2026-09-08)

`php-8.5.9` dynamic build, `--disable-all --enable-fpmng --enable-session
--with-openssl`, no OPcache. 4 static PHP children per backend, 1 nginx worker
or 1 gateway process, `wrk` keep-alive, median of three rotated 10-second runs.
"ok rps" counts only 2xx responses; "rej" is non-2xx/3xx plus socket errors
over all three rounds. Latency is wrk's mixed-outcome percentile, not
successful-only. CPU is measured server-process CPU per successful response
(master + PHP children + nginx/gateway, not the load generator). Raw artifacts:
`benchmark-artifacts/` in the task worktree (not committed).

| scenario | backend | ok rps | rej | cpu/ok µs | p50 ms | p99 ms |
|---|---|---:|---:|---:|---:|---:|
| tiny-c1 | nginx-fastcgi | 5 703 | 0 | 169 | 0.167 | 0.3 |
| tiny-c1 | http | 7 140 | 0 | 126 | 0.135 | 0.2 |
| tiny-c1 | http-direct | **8 377** | 0 | 105 | 0.110 | 0.2 |
| tiny-c32 | nginx-fastcgi | 17 616 | 0 | 163 | 1.82 | 2.1 |
| tiny-c32 | http | 5 015 | 714 509 | 283 | 1.03 | 2.0 |
| tiny-c32 | http-direct | **35 616** | 9 | 112 | 0.74 | 2.4 |
| cpu-c1 | nginx-fastcgi | 1 041 | 0 | 917 | 0.92 | 2.0 |
| cpu-c1 | http | 1 161 | 0 | 803 | 0.81 | 1.5 |
| cpu-c1 | http-direct | **1 309** | 0 | 724 | 0.73 | 1.3 |
| cpu-c32 | nginx-fastcgi | 3 721 | 0 | 1 130 | 8.6 | 10.3 |
| cpu-c32 | http | 2 172 | 813 632 | 1 618 | 0.94 | 2.9 |
| cpu-c32 | http-direct | **4 433** | 0 | 748 | 7.5 | 16.2 |

Measured takeaways: HTTP-direct delivered the highest successful throughput and
the lowest CPU per response in every scenario (roughly +19-102% over nginx+FastCGI
and +19-610% over the `http` gateway on this box). The `http` gateway's high
raw request rate at concurrency 32 consists mostly of 503 rejections; its
successful-response counts are the lowest. HTTP-direct's p99 at cpu-c32 was the
worst (16 ms), consistent with the documented single-event-loop-per-worker and
accept-fairness limits (tasks 055). Single-box, no OPcache, tiny scripts:
not a production forecast.
