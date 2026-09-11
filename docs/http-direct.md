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
A header name longer than 1024 bytes cannot become an `HTTP_*` key and the
request is refused with 400 rather than served with the header missing.

The same variables reach `fpmng_worker_request_env()` under
`pool.executor = worker`: both executors derive the request through
`sapi/fpmng/fpm/fpm_http_direct_request.c`, so what differs between them is
who owns the event loop, not what a request looks like.

Under `pool.executor = worker` the script also gets `STDIN`, `STDOUT` and
`STDERR` (issue #73), the same three the script-running pool types get (issue
#126) and for the same reason: nothing else in an FPM process registers them,
so `fwrite(STDERR, ...)` — what a bridge reaches for to report a failed
handler — used to be a fatal `Undefined constant`. They go where the child's
descriptors already go: the master's pipes under `catch_workers_output = yes`,
`/dev/null` without it, with stdin the master's `/dev/null` and therefore an
immediate EOF. `echo` has always taken that same route in a worker. A line
that must reach the error log regardless of `catch_workers_output`, and with a
severity, still belongs in `error_log()` (issue #124).

Bodies (including chunked requests) are buffered by libevent and supplied through
SAPI, supporting forms, raw `php://input`, and PHP's normal POST handling. PHP
supplies response status, headers (including repeated Set-Cookie), and body through
SAPI. The transport owns Content-Length/Transfer-Encoding and supports HEAD and
HTTP/1.x keep-alive.

## Deliberate limits

- No static files, trusted-proxy handling, gateway ACLs, gateway access log,
  HTTP/2, WebSocket upgrades, or CONNECT/TRACE. TLS is supported — see below.
- Only `http.front_controller`, `http.max_body`, `http.read_timeout`, the
  `http.tls_*` group and — on `pool.executor = classic` only — `http.stream`
  and `http.stream_write_timeout` from the gateway's `http.*` directives are
  accepted. Other
  gateway options are rejected, even if explicitly set to an otherwise harmless
  default. `listen` is the HTTP endpoint; `http.listen` does not apply.
- `chdir` must be absolute; chroot and `listen.allowed_clients` are rejected.
  FPM ping/status listeners and the FastCGI access log are not implemented here;
  use a separate `pool.type = status` for the FPM scoreboard.
- The FastCGI-specific `.user.ini` / per-host/per-directory php.ini activation
  hook is not used. Use php.ini and the pool's `php_value` / `php_admin_value`.
- Requests have a 64 KiB header limit and a body limit of 32 MiB by default,
  configurable down to one byte. Body size cannot be zero/unlimited or above 32 MiB.
- By default responses are buffered until PHP shutdown, capped at 8 MiB body and
  64 KiB forwarded headers. Overflow produces HTTP 500 instead of a partial
  response. `flush()` freezes PHP headers but **does not stream to the client**.
  `http.stream = yes` lifts the body cap and makes `flush()` reach the wire — see
  [Streaming responses](#streaming-responses-httpstream). The header cap applies
  either way. `fastcgi_finish_request()` is unavailable, not a misleading no-op.
- The 64 KiB header cap counts the bytes that reach the wire, not the bytes the
  application set: `name: value\r\n` per emitted header, and nothing for a
  dropped framing header or for `Status:`, neither of which is written. Both
  executors charge it through one function
  (`fpm_http_direct_header_charge()`), so the same response is served or
  refused whichever one runs it (issue #104). Headers the transport itself adds
  (`Date`, `Server`, `Content-Length`) are outside the cap.
- A response header name must be an RFC 9110 token, on both executors and from
  the same check (`fpm_http_direct_header_name_ok()`). `header()` itself rejects
  only CR, LF and NUL in the line, so a name containing a space or a tab, or an
  empty name (`header(': v')`), reaches the transport; the answer is 500, rather
  than a malformed header line on the wire or a header the application asked
  for silently missing. How the 500 is reported does differ: `pool.type =
  http-direct` names the cause in the response body and logs the offending
  header as a `WARNING`, which needs `catch_workers_output = yes` to reach the
  error log at all (issue #73), while under `pool.executor = worker`
  `fpmng_worker_respond()` returns `false` — the handler is the one that
  learns — and the client gets libevent's bare 500 page.
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

## Streaming responses (`http.stream`)

Off by default. With `http.stream = yes` on `pool.type = http-direct` and
`pool.executor = classic`, the worker hands the body to the client as the script
produces it, using chunked transfer encoding, instead of holding the whole
response until PHP shutdown (issue #56).

```ini
http.stream = yes
http.stream_write_timeout = 10000   ; ms, default 10000, must be > 0
```

What changes:

- The response starts on the wire as soon as the script has produced more than
  64 KiB (`FPM_DIRECT_STREAM_CHUNK` in `fpm_http_direct.c`) or calls `flush()`.
  From that moment the status line and the headers are committed.
- The 8 MiB response cap no longer applies: the memory a streamed response costs
  is the unwritten remainder, bounded by 256 KiB
  (`FPM_DIRECT_STREAM_HIGHWATER`), not the response size.
- Backpressure is synchronous. Once more than the high-water mark is still
  unwritten, the script blocks in the SAPI write until the client has taken
  enough of it. This worker serves nobody else meanwhile — that is the same
  property the buffered path already has, applied to a slower phase.
- Nothing streams that cannot: a non-final status (`1xx`), a bodyless response
  (HEAD, 204, 205, 304), or a response already doomed by the header caps stays
  on the buffered path, so the existing framing and error behaviour is unchanged.
  The task 054 tests measure the default and are unmodified.

**Failure mode when the client stalls.** A client that stops reading is given
`http.stream_write_timeout` milliseconds. On expiry — or on any write error, or
if the peer closes — the worker logs a `WARNING`
(`the client stopped reading the response`), drops the buffered remainder and
shuts the socket down **without the terminating chunk**. The client therefore
sees an unterminated chunked message and can tell the response is incomplete; it
never sees a silently short 200. The worker's memory does not grow after the
abort: output from the still-running script is discarded, and the script runs to
its normal shutdown.

**`http.stream` cannot be combined with `http.tls_cert`**; the pool is rejected
at startup. To write from inside a running request without re-entering the event
loop — libevent refuses a reentrant `event_base_loop()` on the base it is
already dispatching from — the writer drives the connection's own descriptor
directly, which is only correct while the descriptor and the bufferevent carry
the same bytes. On a TLS connection they do not. libevent 2.1.12 offers no way
out: `be_openssl_flush()` is an unimplemented stub
(`bufferevent_openssl.c:1259`) and `bufferevent_base_set()` refuses a non-socket
bufferevent.

### Measured results (Intel i7-6700T, 8 threads, poligon, 2026-09-11)

`build/benchmark-http-direct-stream.py`, php-8.5.9, one static worker per pool,
`output_buffering = 0`, loopback client, median of 15 rounds. Peak RSS is the
pool's largest resident size sampled from `/proc` every 5 ms while the response
is in flight. Raw data: `metadata.json` / `results.json` in the scratch folder.

| response | pool | status | TTFB | total | peak RSS |
| --- | --- | --- | --- | --- | --- |
| 4 MiB | buffered | 200 | 2.68 ms | 4.71 ms | 29.6 MiB |
| 4 MiB | streamed | 200 | **0.23 ms** | 1.95 ms | **25.7 MiB** |
| 16 MiB | buffered | 500 (over the 8 MiB cap) | 5.78 ms | 5.78 ms | 33.4 MiB |
| 16 MiB | streamed | 200 | **0.18 ms** | 8.23 ms | **25.7 MiB** |

Time to first byte is ~12x lower at 4 MiB and does not grow with the response,
which is the point: it is the time to the first chunk, not to the last. Peak RSS
is flat across both sizes for the streamed pool and grows with the response for
the buffered one; at 16 MiB the buffered pool has no answer at all. The
loopback client never stalls, so these numbers do not exercise backpressure —
`sapi/fpmng/tests/fpmng-http-direct-streaming.phpt` does.

## TLS

A direct pool terminates TLS itself, on both executors, using the same
implementation the `http` gateway uses (`fpm_http_tls.c`,
`fpm_http_tls_reload.c`) — no second TLS stack (issue #55).

```ini
[app]
listen = 0.0.0.0:8443
pool.type = http-direct
http.tls_cert = /etc/ssl/app/fullchain.pem
http.tls_key = /etc/ssl/app/privkey.pem
http.tls_min_version = TLSv1.2      ; optional
http.tls_sni_cert = alt.example:/etc/ssl/alt/fullchain.pem:/etc/ssl/alt/privkey.pem
http.tls_reload_check = 5           ; seconds; 0 turns reload off
```

What this means in practice:

- **One socket, one protocol.** `listen` either speaks TLS or it does not.
  There is no `http.plain_listen` for a direct pool — the directive is rejected
  rather than ignored, because a direct pool accepts on exactly one socket and
  there is nowhere to put a second listener. A plain-HTTP client talking to a
  TLS pool fails the handshake; it is never served cleartext.
- **`REQUEST_SCHEME` is `https` and `HTTPS` is `on`** for every request such a
  pool serves, on both executors, from the same code that builds the rest of
  the environment (`fpm_http_direct_build_env()`). On a plain pool
  `REQUEST_SCHEME` is `http` and `HTTPS` is absent — CGI has no negative form
  for it, and PHP reads any non-empty value as on.
- **The private key is read once, in the master, before the first child
  forks.** No child ever opens it from disk, and no `SSL_CTX` is inherited
  through `fork()`: each child builds its own.
- **Reload without a restart.** The master digests `http.tls_cert` /
  `http.tls_key` every `http.tls_reload_check` seconds and publishes a new
  generation when the *content* changes (a digest, not `st_mtime`, so two
  writes in one second are distinguishable — issue #71). Children pick the new
  generation up on their own tick and use it for connections accepted from
  then on; a connection already handshook finishes on the certificate it
  started with. A child respawned after a reload starts on the newest
  certificate, not the startup one (issue #91). A candidate that does not parse
  or whose key does not match is logged and *not* installed — the certificate
  already in use keeps serving.
- **Bad configuration fails at startup, never at request time.** A missing
  `http.tls_key`, an unreadable path, a key that does not match its
  certificate, an unknown `http.tls_min_version`, or `http.tls_cert` on a build
  without OpenSSL / libevent's OpenSSL glue all make `php-fpm-ng` refuse to
  start, with a message naming the pool and the problem. Refusing is
  deliberate: an operator who configured a certificate asked for HTTPS on that
  port, and quietly serving plain HTTP there instead is the one outcome that
  must not happen.

Not covered here: client-certificate verification and exposing the peer
certificate to PHP (issue #62), OCSP stapling, session-ticket rotation beyond
the shared key generated at startup, and ACME issuance/renewal (issue #46).

Covered by `sapi/fpmng/tests/fpmng-http-direct-tls.phpt`: a handshake and a
request on each executor, the CGI variables, plain HTTP refused on the TLS
port, a reload picked up by new connections while one open across the swap
keeps working, and the master not restarting.

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
