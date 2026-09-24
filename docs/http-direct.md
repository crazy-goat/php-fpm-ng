# Experimental direct HTTP: classic PHP, static FPM pool

`pool.type = http-direct` runs libevent's HTTP parser and PHP in each FPM child.
It does **not** start gateway children, create an internal FastCGI listener, or
serialize requests/responses as FastCGI records. The master still creates,
monitors, replaces, and signals normal FPM children.

This is a **POC, not a production frontend**. Existing `http` and `fastcgi`
pools are unchanged.

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
`ondemand` fail configuration validation
(`sapi/fpmng/fpm/fpm_http_direct_request.c:126-127`). No fibers, persistent
application container, or shared request state: every request runs PHP
startup, the script, and PHP shutdown, including extension RINIT/RSHUTDOWN.

**Decision (2026-09-13, spike #66, measurement #169, write-up #170):** this
restriction stays. It is not because a retired direct child would cost more
memory than it saves — the fiber executor (branch `async`) was not the question here, and
this build's children were too small (~2 MB PSS per child) for the saving to
matter either way. It stays because retiring a direct child today drops any
request that was in flight on its socket: every measured retirement with a
request in flight lost it (`n_conn_closed_without_response ==
inflight_requests_at_t0` in all 24 such rows, #169). `pm = dynamic` and
`ondemand` retire children routinely as load falls; `pm = static` does not.
Making the routine case exercise a path that already drops in-flight requests
is not something the gate should allow yet. The condition that reopens this:
#310 and #311 (the two mechanisms behind that drop, per
`docs/spike-direct-pool-pm-report.md`) both land, clearing rule 3 in
`build/benchmark-http-direct-pm.md` — see that report for the full reasoning
and for what would still need measuring even then (rules 1/2/4, not evaluated
here for lack of a legal `dynamic`/`ondemand` row to compare).

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

The daemon's **own** lines are a different matter and do not depend on that
setting. A direct child owns the accept socket, so it is the only process that
knows it has stopped accepting: retiring (issue #65), the end of a drain, and
the once-per-child limit NOTICEs are narrated by the child or by nobody. Since
issue #260 those go to the master over the per-pool channel of issue #121 and
land in `error_log` at their own level with `(child N)` appended, with
`catch_workers_output` left at its default. What the setting still governs is
what the *application* writes to its own stdout and stderr, which is what its
documentation says it is for.

PHP's own diagnostics deliberately do **not** change here. An http-direct child
has a response to display errors in, so `display_errors` from `php.ini` means
in this pool type what it has always meant. (A `pool.type = supervisor` or
`cron` child has no response, which is why those two types also take the INI
half of issue #124 — see `fpm_pool_type.h`.)

Bodies (including chunked requests) are buffered by libevent and supplied through
SAPI, supporting forms, raw `php://input`, and PHP's normal POST handling. PHP
supplies response status, headers (including repeated Set-Cookie), and body through
SAPI. The transport owns Content-Length/Transfer-Encoding and supports HEAD and
HTTP/1.x keep-alive.

## The listening socket

`listen` is the pool's own socket, opened by the master before the children fork
and shared by all of them. The master sets `TCP_NODELAY` on it before forking,
and Linux copies the option onto every connection accepted through it. (Before
the fork, not in the child: the copy happens when the handshake completes, so a
child setting the option during its own start-up would leave whatever was
already queued behind it on the old setting.)

That is not a tuning knob but a correctness one. FPM's listener code was written
for FastCGI, where the peer is a web server on the same host; here the peer is
the client, and without `TCP_NODELAY` the trailing partial segment of every
response over a few kilobytes waits for the client's delayed ACK. Measured on a
256 KiB response before issue #244 fixed it: 31 requests per second with a p99
of 43 ms, against 4282 and 0.32 ms after. The `http` gateway never had this — it
sets the option on its own listener.

## Sharing a burst across workers

Every child accepts on the one listening socket the master opened, so the kernel
decides which child serves a connection. libevent's listener, though, accepts in
a loop until the queue is empty: the child that wakes first used to take an
entire burst into its own connection list and then serve it one request at a
time, with its event loop blocked for the whole of each, while its siblings sat
idle. On `pool.executor = classic` a single slow request therefore delayed every
connection that happened to land on the same child, by as long as that request
took.

Since issue #53 each child stops accepting as soon as it has accepted one
connection, and starts again when that connection's request ends. libevent
re-checks the listener's enabled flag after each accept callback, so the drain
stops there and the rest of the queue is left for whichever child wakes next.
The hook is `evhttp_set_bevcb()`, the only per-connection hook evhttp exposes;
on a TLS pool it is `fpm_http_direct_tls.c` that owns the bevcb and the gate
travels with it, so a certificate reload cannot drop it. A 10 ms timer re-opens
accepting for a child that accepted a connection and was then told nothing, so a
silent peer cannot wedge a child out of accept until the read timeout.

The gate is not a scheduler and makes no guarantee about the distribution. A
child whose request completes in microseconds re-opens accepting and can take a
second connection out of the same burst, including a slow one; what it removes
is the case where one child takes *everything*. `pool.executor = worker` does
not use it: there the event loop is driven by userland and can hold several
requests in flight, so the same gate would be a throughput cost against a
different, and so far unmeasured, problem.

### Measured results (Intel i7-6700T, 8 threads, poligon, 2026-09-11)

`build/benchmark-http-direct-fairness.py`, 8 connections, three repeats per
cell, both binaries built by `build/libphp-build.sh` against the same
distribution libphp and run back to back. Three workloads: a burst released by a
barrier, 10 s of keep-alive traffic, and one 1000 ms request among seven fast
ones.

| workers | metric | before | after |
| --- | --- | --- | --- |
| 4 | burst, workers used | 3/4, 2/4, 2/4 | 4/4, 4/4, 4/4 |
| 4 | keep-alive, busiest worker's share | 0.373, 0.624, 0.875 | 0.377, 0.374, 0.375 |
| 4 | slow peer, fast requests served off the sleeper | 7/7, 2/7, 7/7 | 7/7, 7/7, 6/7 |
| 4 | slow peer, slowest fast request | 1.21, 1003, 1.04 ms | 1.17, 1.38, 1001 ms |
| 2 | keep-alive, busiest worker's share | 0.623, 0.500, 0.744 | 0.625, 0.623, 0.745 |
| 2 | slow peer, fast requests served off the sleeper | 6/7, 3/7, 1/7 | 7/7, 7/7, 7/7 |
| 2 | slow peer, slowest fast request | 1001, 1003, 1005 ms | 1.38, 1.53, 1.46 ms |

The cost, from the same runs, as the median of three keep-alive repeats: 34773
to 34929 requests at one worker (+0.4%), 34316 to 34191 at two (-0.4%), 33205 to
32559 at four (**-1.9%**). Interrupting the accept drain is not free, and at four
workers it is outside the run-to-run spread.

Two things the table does not show, recorded because they cost time to learn.
An earlier attempt closed the gate only while PHP ran and measured as noise: in a
burst every child is idle, so the accept storm is over before any PHP starts and
the window it closed was never open. And a first comparison against a
separately-built baseline reported a 3-5% throughput cost that turned out to be
the two binaries, not the change -- at one worker, where the gate cannot affect
anything, that baseline differed from the gated binary by 70%. Both numbers
above come from one sitting on one toolchain for that reason.

`sapi/fpmng/tests/fpmng-http-direct-accept-fairness.phpt` guards the behaviour.

## Deliberate limits

- No trusted-proxy handling, gateway ACLs, gateway access log, HTTP/2, or
  CONNECT/TRACE. WebSocket upgrades are supported on `pool.executor = worker`
  only — [WebSocket](#websocket-poolexecutor--worker-issue-343); the classic
  executor keeps refusing them, and TLS is supported — see below. Static
  files are too, opt-in and on the classic executor only — see below as well.
  The pool-level operator directives (`ping.path`, `operator.status_path`,
  `access.log`, `listen.allowed_clients`, `chroot`) are supported as well — see
  [Operating a direct pool](#operating-a-direct-pool).
- Only `http.front_controller`, `http.max_body`, `http.read_timeout`, the
  `http.tls_*` group and — on `pool.executor = classic` only — `http.stream`,
  `http.stream_write_timeout`, `http.static`, `http.max_connections` and
  `http.max_connections_per_client` from the gateway's `http.*`
  directives are accepted. Other
  gateway options are rejected, even if explicitly set to an otherwise harmless
  default. `listen` is the HTTP endpoint; `http.listen` does not apply.
- `chdir` must be absolute.
- `.user.ini` **is** read, and the directory that governs is the front
  controller's — never anything the client sent (issue #60). The pool scans
  from its document root (the resolved `chdir`) down to the directory holding
  the resolved `http.front_controller`, exactly the shape the CGI SAPI scans,
  with the request removed from it. Both ends are pool configuration resolved
  once per child, so a traversal-style URI, a URI naming some other directory's
  `.user.ini`, and a URI naming no path at all all produce the same ini set.
  It is **on by default**, governed by the same php.ini settings as everywhere
  else: `user_ini.filename` (set it empty to turn `.user.ini` off) and
  `user_ini.cache_ttl`. Default-on because the surprising pool would be the
  other one — moving a pool from FastCGI to http-direct with the same php.ini
  keeps the same `.user.ini` in force. Caching follows the CGI SAPI's TTL, with
  one addition: on expiry each candidate file is `stat()`ed and re-parsed only
  if it actually changed, so an unchanged deployment never re-parses.
  `[PATH=...]` sections from php.ini are activated against the same directory.
  Per-**host** activation (`[HOST=...]`) is deliberately not done: it is keyed
  on `SERVER_NAME`, which here is the client's `Host` header.
  On `pool.executor = worker` one `php_request_startup()` covers the whole
  worker, so the hook fires once and the `.user.ini` next to the worker script
  governs every request that worker then serves.
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
  header as a `WARNING`, which since issue #260 reaches `error_log` like any
  other line the child writes, while under `pool.executor = worker`
  `fpmng_worker_respond()` returns `false` — the handler is the one that
  learns — and the client gets libevent's bare 500 page.
- There are at most 16 pending PHP response writes per worker; further ready
  requests get 503. This is **not a total connection/memory bound**: partially
  read requests and idle connections also use memory. Since issue #61 the
  number of connections a worker holds can be capped — see
  [Connection limits](#connection-limits-httpmax_connections) — but that is a
  connection count, still not a memory bound.
- `http.read_timeout` must be positive. It is libevent's **inactivity** timeout
  for reads/writes, plus — since issue #61 — an absolute deadline on the
  **first** request of a connection, see
  [Connection limits](#connection-limits-httpmax_connections). A trickling
  client can still evade the inactivity timer on the second and later requests
  of a keep-alive connection. PHP execution blocks this worker's event loop,
  including its timers.
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

### Held requests under `pool.executor = worker` (decision, 2026-09-11)

A worker may hold **at most `worker.max_pending` accepted-but-unanswered
requests at a time** (default 256, `FPM_WORKER_PENDING_MAX` in
`sapi/fpmng/fpm/fpm_http_direct_worker.h`). That is the concurrency limit of
deferred replies, and it is a hard one: reaching it does not merely throttle,
it retires the worker.

The transport cannot tell a request held on purpose — the long-poll that
`fpmng_worker_respond()` exists to answer later — from one leaked by a handler
that returned without answering, because a pending entry is removed only by
`fpmng_worker_respond()` (`fpm_worker_reap()`) or, once it expires,
`worker.request_timeout` (below). **This decision resolves that ambiguity
against the deliberate case, on purpose**: past the ceiling,
`fpm_worker_accept()`

- answers the request that tipped it over `503 Worker unavailable`,
- logs `WARNING … N requests accepted but unanswered`,
- sets the stop flag, so the worker script is asked to stop and the master
  respawns the child — and `fpm_worker_finish_output()` then answers **every
  one of the held requests** `503` as well, logging how many it abandoned.

So a handler that deliberately parks `worker.max_pending` long-polls loses all
of them. The alternative — a way for the script to mark a request as held on
purpose (`worker.on_saturation = stop|reject`, or similar) — is not
implemented; this decision is only about making the ceiling itself
configurable and giving a stuck handler a way out via a timeout, not about
changing what happens once the ceiling is reached.

Two consequences follow from the same "the child never ends a request"
property and are **not** bugs in the above, but they are easy to trip over:

- `pm.max_requests` counts only *answered* requests, so a worker that holds
  requests forever never recycles on that trigger. The pending ceiling (and,
  per-request, `worker.request_timeout`) are the only things that eventually
  free it.
- The child reports `ACCEPTING` for its whole life
  (`fpm_request_accepting(false)` is called once), so `fpm_request_is_idle()`
  (`sapi/fpmng/fpm/fpm_request.c`) — and therefore `pm = ondemand` bookkeeping
  and the scoreboard — see a worker holding long-polls as idle. Per-request
  accounting for this executor is #64.

If a supported long-polling shape ever needs more than `worker.max_pending`
held requests per worker, or needs them to survive the ceiling, that is a
design change: it has to come with a bound of its own, and it belongs to #68
and its follow-ups, not to this limit.

#### `worker.max_pending` (issue #331)

- **Default:** `256` (`FPM_WORKER_PENDING_MAX`).
- **What it does:** caps how many accepted-but-unanswered requests a single
  worker may hold at once, as described above. Must be a positive integer;
  `0` or a negative value is rejected at configuration validation time
  (`fpm_http_direct_worker_validate()`).
- **Only valid under `pool.executor = worker`.** Every other pool type and
  every other executor of `pool.type = http-direct` rejects it at startup,
  the same way `fiber.*` is rejected outside `pool.executor = fiber`.
- **Example:**

  ```ini
  [pool]
  pool.type = http-direct
  pool.executor = worker
  worker.max_pending = 64
  ```

  A worker in this pool answers `503 Worker unavailable` (and recycles, per
  the decision above) on its 65th concurrently held request instead of its
  257th.
- **Interaction:** lowering it makes the worker recycle sooner under a burst
  of concurrent long-polls; raising it holds more state (one
  `struct fpm_worker_pending` and one ring-buffer slot each) in exchange for
  tolerating a larger burst before the safety net above trips.

#### `worker.request_timeout` (issue #331)

- **Default:** `0` (off) — milliseconds, the same convention as
  `http.read_timeout`.
- **What it does:** bounds how long a single accepted request may sit
  unanswered before the SAPI answers it itself. `request_terminate_timeout` is
  rejected for this executor (the worker script never ends a request and the
  scoreboard stage never changes, so nothing there would ever detect it); this
  directive is the equivalent lever for `pool.executor = worker`. Past the
  timeout, the worker answers `504 Gateway Timeout` (not `503`: this is a
  single stuck request being cut loose, not the worker itself being retired)
  and reaps the pending entry the same way `fpmng_worker_respond()` does, so
  its slot counts toward `worker.max_pending` again as soon as the next
  drain of the ring buffer notices it is gone.
- **`0` is a true off**, not merely a no-op check: no sweep timer is armed at
  all when the directive is `0`, so a pool that never sets it pays nothing for
  the feature.
- **Only valid under `pool.executor = worker`,** rejected everywhere else the
  same way `worker.max_pending` is.
- **Example:**

  ```ini
  [pool]
  pool.type = http-direct
  pool.executor = worker
  worker.request_timeout = 5000
  ```

  A handler that never calls `fpmng_worker_respond()` for a given request gets
  that request answered `504` on its behalf after 5 seconds, instead of
  holding a slot for the life of the worker.
- **Implementation note:** a single periodic sweep (armed only when the
  directive is non-zero, at `worker.request_timeout / 4` — floored at 25 ms —
  so the worst-case overshoot past the deadline is one sweep interval, not a
  whole timeout period) walks the pending table and expires overdue entries,
  rather than one libevent timer per pending request. `worker.max_pending`
  defaults to 256 held requests; arming and tearing down that many one-shot
  timers on every accept/respond would be considerably more event-loop
  bookkeeping than one bounded walk every sweep tick. See
  `fpm_worker_sweep_expired()` in `sapi/fpmng/fpm/fpm_http_direct_worker.c`.
- **Interaction:** does not ask the worker to stop or recycle — unlike
  `worker.max_pending` saturation, a timeout answers and frees exactly the
  request that overstayed, and everything else keeps running.

#### `worker.max_memory` and `worker.max_lifetime` (issue #334)

`pm.max_requests` bounds a classic worker's memory growth by counting
*answered* requests. `pool.executor = worker` answers requests too — but it is
also the executor `fpmng_worker_respond_start()`/`_chunk()`/`_end()` (issue
#332) stream a long-poll or an SSE feed through, and a worker holding one of
those can go a long time between answers. Nothing bounded a per-request leak
in that window: `worker.max_memory` and `worker.max_lifetime` close it, the
same way `supervisor.max_memory`/`supervisor.max_runtime` (issues #324/#326)
close it for `pool.type = supervisor` — see `docs/supervisor.md`.

- **`worker.max_memory = <size>`** (for example `256M`, same K/M/G suffix as
  `http.max_body`), default `0` (disabled). A periodic check (armed whenever
  either this directive or `worker.max_lifetime` is non-zero — see the
  implementation note below) reads the worker's own peak resident set size
  (`getrusage(RUSAGE_SELF).ru_maxrss`, converted to bytes the same way
  `supervisor.max_memory` does — see the platform-unit comment in
  `fpm_http_direct_worker.c`); once it reaches the configured limit, the
  worker is asked to stop the exact same graceful way `pm.max_requests` asks
  it to: in-flight work drains (through `fpm_worker_finish_output()`, the same
  drain `worker.max_pending` saturation triggers), the child exits, the master
  respawns it.
- **`worker.max_lifetime = <seconds>`** (same suffix convention as
  `supervisor.max_runtime`), default `0` (disabled). Bounds how long a single
  worker process may run before it is recycled, regardless of memory or
  request volume — the same periodic check compares elapsed time since the
  worker started serving against this limit and triggers an identical
  graceful stop.
- **Both are opt-in and independent.** Setting only one leaves the other
  unbounded, exactly today's behavior.
- **Neither is a kill.** Unlike `supervisor.max_runtime`, which sends
  `supervisor.stop_signal` to a *script it does not otherwise control the
  shape of*, this executor already has a well-defined graceful stop
  (`fpm_worker_stopping`, the same flag `pm.max_requests` and
  `worker.max_pending` saturation set): a trip here sets that flag and lets
  the worker script drain and exit on its own, never `kill()`. This executor
  has no per-request state isolation, so cutting a worker off mid-flight would
  lose whatever it was holding — exactly the outcome `worker.request_timeout`
  and `worker.max_pending`'s own drains already avoid.

**The stop request is cooperative.** The master signals the worker (SIGQUIT on
reload); `fpmng_worker_stopping()` / `fpmng_worker_may_exit()` let the booted
script's event-loop driver notice and drain. They do not preempt PHP code. A
script that never checks them can keep running until the master escalation:
reload sends SIGTERM after the global `process_control_timeout`, then SIGKILL
one second later if necessary. SIGTERM is normally the worker's immediate
termination action; a script can install a handler, so the final SIGKILL is the
hard bound. Master termination starts with SIGTERM and escalates to SIGKILL
after `process_control_timeout`. This is bounded by the existing global policy,
not by a worker-specific timer; `worker.request_timeout` applies to unanswered
requests and does not bound the worker script itself. See
[`shutdown-timeouts.md`](shutdown-timeouts.md#directives-by-pool-type).
- **Only valid under `pool.executor = worker`,** rejected everywhere else the
  same way `worker.max_pending` is.
- **Example:**

  ```ini
  [pool]
  pool.type = http-direct
  pool.executor = worker
  worker.max_memory = 256M
  worker.max_lifetime = 3600
  ```

  A worker in this pool recycles once its peak RSS reaches 256 MiB, or after
  an hour of uptime, whichever comes first.
- **Implementation note:** a single periodic timer (`fw.health_sweep`,
  one second, independent of `worker.request_timeout`'s own sweep — that one
  is armed only when `worker.request_timeout` itself is non-zero, so it is not
  reliably running) checks both directives together, the same one-timer-not-
  one-per-request shape `worker.request_timeout`'s sweep already uses. See
  `fpm_worker_health_sweep()` in `sapi/fpmng/fpm/fpm_http_direct_worker.c`.
- **Interaction:** a memory or lifetime recycle is logged (`NOTICE`, mirroring
  `supervisor.max_memory`'s wording) but is not otherwise different from any
  other graceful stop of this executor — `worker.max_pending` saturation,
  `pm.max_requests`, SIGQUIT, or a reload all drain and recycle the same way.

#### `worker.accept_threshold` (issue #338)

Every child of an `http-direct` pool accepts from the one listening socket the
master opened, and libevent accepts until the kernel's queue is empty — so the
child that wakes first takes the whole backlog. For `pool.executor = classic`
issue #53 answered that with an accept gate that stays shut for the whole of a
request. This executor never had one, on the grounds that a gate shaped like
classic's would be shut for good here: a worker holds many requests at once by
design. What it got instead was a ceiling.

- **Default:** `1` (`FPM_WORKER_ACCEPT_THRESHOLD`). `0` disables it and restores
  the "accept the whole backlog" behaviour measured below.
- **What it does:** a worker accepts at most this many connections, then
  disables its own listener for `FPM_WORKER_ACCEPT_COOLDOWN_MS` (5 ms) — so the
  rest of the accept queue stays there for a sibling that is doing nothing. It
  is a rate on the accept path, one connection per 5 ms by default, and nothing
  else: a worker at its ceiling keeps serving every connection it already has,
  including new requests arriving on them.
- **It does not cap what a worker holds.** An in-flight ceiling — refuse to
  accept while holding N unanswered requests, which is what classic's gate
  amounts to — was tried first and rejected: it caps concurrency *inside* one
  worker, which is this executor's whole point.
  `fpmng-http-direct-worker.phpt`'s four overlapping one-second requests
  serialised under it. Fairness between workers is not worth buying with
  concurrency inside one. Use `worker.max_pending` for how much a worker may
  hold.
- **Why the cooldown.** It is not decoration, it is the half that works. A
  worker that answers in microseconds is idle again long before the kernel has
  scheduled any sibling the same connection woke, so a gate that reopens as soon
  as the worker is free loses the race to its own incumbency every time. The
  first implementation of this directive did exactly that — accept at most N per
  `fpmng_worker_loop()` iteration, reopen on the next one — and moved the
  busiest worker's share of a 64-connection keep-alive run from 0.51 to 0.56,
  which is noise. Classic's gate has the same shape in its 10 ms
  `fpm_direct_tick` safety timer.
- **Only valid under `pool.executor = worker`,** rejected everywhere else the
  same way `worker.max_pending` is.
- **Example:**

  ```ini
  [pool]
  pool.type = http-direct
  pool.executor = worker
  worker.accept_threshold = 1
  ```

- **Measured.** `build/benchmark-http-direct-fairness.py --executor worker`,
  8 workers, 64 keep-alive connections, 60 s, three repeats — the shape issue
  #53 used for classic. `busiest` is the share of all requests served by the
  busiest worker (`0.125` is a perfect split over eight), `used` how many of the
  eight workers served anything at all:

  | run | `worker.accept_threshold` | busiest | least busy | workers used | requests |
  | --- | --- | --- | --- | --- | --- |
  | keep-alive | unset (`0`) | 0.640 / 0.359 / 0.812 | 0.000 / 0.000 / 0.000 | 5 / 3 / 3 | 1 322 479 / 1 442 354 / 1 449 420 |
  | keep-alive | `1` (the default) | 0.125 / 0.125 / 0.125 | 0.125 / 0.125 / 0.125 | 8 / 8 / 8 | 1 413 020 / 1 421 417 / 1 439 925 |
  | keep-alive | `pool.executor = classic`, for reference | 0.172 / 0.171 / 0.156 | 0.078 / 0.094 / 0.094 | 8 / 8 / 8 | 1 044 018 / 1 017 030 / 1 067 921 |

  A perfectly even split over eight workers is what the default measured, three
  runs out of three — with the directive unset, three to five workers carried
  the whole pool and the rest never saw a connection.

  Throughput is not the cost here that it was for classic (issue #53 measured
  −1.9 %): the mean of the three keep-alive runs went from 1 404 751 to
  1 424 787 requests, +1.4 %, because eight workers sharing the load evenly beat
  three workers carrying it. The same shape at 4 workers and 8 connections moves
  from busiest 0.500 / 0.875 / 0.503 over 4 / 2 / 2 workers to 0.376 / 0.251 /
  0.251 over 4 / 4 / 4, and from 59 864 requests to 121 018.
- **Why `1` is the default.** Measured over the same shape (20 s runs) at `0` /
  `2` / `8`, the busiest share on the keep-alive workload was 0.62 / 0.141 /
  0.141 against the default's 0.125: what spreads the load is the cooldown, not
  the size of the ceiling, so every non-zero value measured fair and the
  differences between them are small. `1` is the default because it is the most
  conservative of them and the only one that measured the ideal split exactly;
  raise it for a pool whose connections are short-lived enough that a
  one-connection-per-5 ms accept rate per worker is the binding constraint.
- **Interaction:** this is a per-worker, per-process decision only. No shared
  memory, no cross-worker wakeup, and the listening socket is still the single
  one the master opened — `SO_REUSEPORT` is deliberately not used, since it
  would break the retire contract documented above. A worker blocked in PHP
  stops accepting anyway, because its event loop is not running; the ceiling is
  about the worker that is *not* blocked and would otherwise take everything.
  The rate it imposes is per worker, so a pool of N children still accepts
  N × `worker.accept_threshold` connections per 5 ms; raise the directive for a
  pool whose connections are short-lived enough that new-connection latency
  matters more than keep-alive pinning does.
  `fpmng-http-direct-worker-saturation-refuses-new.phpt` sets it to `0` for the
  one thing the rate does change: which event-loop iteration a connection is
  accepted in, and therefore the order two nearly simultaneous connections are
  served in.

### Streaming responses under `pool.executor = worker` (issue #332)

`fpmng_worker_respond()` takes one complete body: nothing reaches the wire
until the handler has the whole response in hand. Three more builtins mirror
classic's `http.stream` for this executor, adapted to its cooperative
event-loop model — `fpmng_worker_loop()` already drives the base between calls
into PHP, so there is no need for `http.stream`'s busy-pump writer:

```php
fpmng_worker_respond_start(int $id, int $status, array $headers): bool;
fpmng_worker_respond_chunk(int $id, string $data): bool;
fpmng_worker_respond_end(int $id): bool;
```

- **`fpmng_worker_respond_start()`** puts the status line and headers on the
  wire with `Transfer-Encoding: chunked` framing and marks the pending entry as
  streaming. It refuses (returns `false`, never throws for a refusal reason —
  only a malformed `$status` throws, the same `fpm_http_direct_status_final()`
  check `fpmng_worker_respond()` already uses) when:
  - the id is unknown, its connection is already gone, or it is already
    streaming (a second `_start()` on the same id would be a second status
    line for one request);
  - the client is **HTTP/1.0** — there is no chunked framing to give it, the
    same reason classic refuses to stream to one;
  - the status is bodyless (`204`, `205`, `304`, or the request was `HEAD`) —
    `fpm_http_direct_status_bodyless()`, the same check classic uses.

  A malformed header name is refused the same way `fpmng_worker_respond()`
  refuses one: the connection's headers are dropped, the client gets a bare 500
  from libevent, and the pending entry is reaped — `_start()` reports `false`.
  A caller-supplied `Content-Length`, `Transfer-Encoding`, `Connection`, etc. is
  not one of the refusal reasons above — like `fpmng_worker_respond()`, this
  transport owns framing and silently drops those headers rather than refusing
  the whole call (`fpm_http_direct_header_dropped()`).
- **`fpmng_worker_respond_chunk()`** writes one piece of the body. It returns
  `false`, and queues nothing, when the id was never started (or already
  ended), the connection is gone, or `worker.send_buffer_limit` (below) is set
  and the piece would push the connection's queued-but-unwritten output past
  it. An empty string is a no-op that still returns `true` — calling libevent's
  chunk writer with zero bytes would send the terminating chunk early.
- **`fpmng_worker_respond_end()`** writes the terminating chunk, then does
  exactly the bookkeeping `fpmng_worker_respond()`'s tail already does: drop
  the close callback, count the reply so a shutting-down worker waits for it to
  reach the wire, reap the pending entry (freeing its `worker.max_pending`
  slot), and recycle the worker if this answer reached `pm.max_requests`. It
  returns `false` under the same conditions as `_chunk()`.

**Backpressure: `worker.send_buffer_limit`.** Bytes (`64K`, `1M`, … or a plain
byte count, parsed the same way as `http.max_body`), default `0` (off, no
bound). Once more than this many bytes are queued but not yet written to a
connection's socket, `fpmng_worker_respond_chunk()` refuses to queue more —
returning `false` rather than throwing, or blocking, or growing the queue
without limit. Unlike `http.stream`'s synchronous high-water write (which
blocks the whole worker until the client catches up), a refusal here is just a
signal: the handler decides what to do with it — retry later, drop the
connection itself, or apply its own flow control. Only valid under
`pool.executor = worker`; every other pool type and executor of
`pool.type = http-direct` rejects it, the same way `worker.max_pending` and
`worker.request_timeout` are rejected outside this executor.

```ini
[pool]
pool.type = http-direct
pool.executor = worker
worker.send_buffer_limit = 256K
```

**Interaction with `worker.request_timeout`.** That directive means "never got
its first byte of response", not "streaming took a while": once
`fpmng_worker_respond_start()` has run, `worker.request_timeout`'s sweep
(`fpm_worker_sweep_expired()`) skips the entry, and a stream may legitimately
run for as long as the client keeps reading. The one remaining backstop is
`http.read_timeout` — already the transport's per-connection libevent
read+write inactivity timeout for every request on this executor, and, until
this issue, undocumented for `pool.executor = worker` specifically: a
connection that stops reading (or writing) entirely for that long is dropped
by libevent regardless of `worker.send_buffer_limit`, the same as any other
connection this executor holds.

**Interaction with `worker.max_pending`.** A streaming request still counts as
one held pending entry for its whole duration, from `_start()` to `_end()` (or
until its connection closes): it occupies the same slot an unanswered
`fpmng_worker_respond()` request would. A handler that streams
`worker.max_pending` responses at once and keeps them all open saturates the
worker exactly as a long poll would.

**Interaction with `pm.max_requests` and graceful shutdown.** A stream in
progress when the worker is asked to stop (`pm.max_requests` reached, or
SIGQUIT/reload) cannot be answered `503`: its status line — and possibly some
chunks — are already on the wire, and `evhttp_send_error()`'s second status
line is not an option once that has happened. `fpm_worker_finish_output()`
instead **ends the stream cleanly** (issue #342): the terminating chunk goes on
the wire, so a client reading a stream — an `EventSource`, for instance — sees
a complete chunked message and reconnects, honouring `Last-Event-ID`, rather
than treating the connection as broken. It still waits for the ending chunks to
reach the wire the normal way, bounded by the same shutdown budget as every
other queued reply. (Until #342 this path cut streams short with a
`shutdown()`, mirroring classic's `fpm_direct_stream_abort()`; the clean end
made that shape unreachable, so it is gone.)

**Server-Sent Events** are the workload this whole section was built for: a
response that is *deliberately never finished*, one `data:` frame per event.
On top of the builtins above an SSE endpoint needs only the three semantics
this file states — a started stream occupies one `worker.max_pending` slot for
as long as the client reads, is exempt from `worker.request_timeout` once
`_start()` has run, is ended with a clean terminating chunk when the worker
retires, and has its dead clients reported by id through
[`fpmng_worker_closed_requests()`](#server-sent-events-sse-issue-342). A
runnable example lives in `examples/http-direct-worker-sse/`.

### Server-Sent Events (SSE) (issue #342)

An SSE stream is a chunked response that is deliberately never finished, which
breaks the assumptions three places in the worker executor were written under
— all three now have decided, tested semantics:

1. **Timeout.** A stream with `fpmng_worker_respond_start()` already called is
   **exempt** from `worker.request_timeout` — that directive means "never got
   its first byte of response", and a stream that sent its last event 30 s ago
   is healthy, not leaked. It is exemption, not per-chunk re-arming: the
   directive keeps exactly one meaning, "how long an unanswered request may
   sit", and a started stream is out of its business. The remaining backstop
   is `http.read_timeout`, as for every connection.
2. **Retirement.** When the worker stops, every still-open stream is ended with
   a clean terminating chunk, not a 503 and not a reset — an `EventSource`
   reconnects automatically. See the "graceful shutdown" interaction above.
3. **Client gone.** A client that walks away mid-stream is reported by request
   id: the notify pipe signals *that* something happened, and

   ```php
   fpmng_worker_closed_requests(): list<int>
   ```

   drains *which* ids died, oldest first, emptying the queue — the same drain
   shape as `fpmng_worker_next_request()`. Without it a fan-out driver learns
   of a dead client only from `_chunk()` returning `false`, which for a stream
   emitting an event every 15 s keeps the dead client's subscription alive for
   up to one heartbeat interval.

Per-stream cost is one `worker.max_pending` slot for the life of the stream: a
pool whose purpose is holding streams should size `worker.max_pending` to its
expected subscriber count, and consider `worker.send_buffer_limit` so a slow
reader cannot queue events without bound. TLS needs nothing special — the SSL
bufferevent the stream writes through is the same one every chunk of every
response uses. SSE frames themselves (`event:`, `data:`, `id:`, retry) are
userland bytes; the transport never parses them.

`examples/http-direct-worker-sse/` is a runnable, dependency-free example: one
`/events` route with a ping comment every 15 s, `Last-Event-ID` honoured from
`$_SERVER['HTTP_LAST_EVENT_ID']` (headers arrive CGI-style via
`fpmng_worker_request_env()`), fan-out of one published message to every open
stream *on the worker* — cross-worker publish/subscribe is a different problem
(issue #182) and deliberately not answered here.

## WebSocket (`pool.executor = worker`) (issue #343)

`fpmng_worker_upgrade(int $id, array $responseHeaders): resource|null` turns a
valid pending request into an ordinary bidirectional PHP stream:

1. Before changing connection ownership, it requires a `GET`, `Upgrade:
   websocket`, a `Connection` token list containing `Upgrade`, exactly one
   canonical base64 `Sec-WebSocket-Key` decoding to 16 bytes, and exactly one
   `Sec-WebSocket-Version: 13`.
2. A malformed method/upgrade/Connection/key is answered by the transport with
   `400 Bad Request`. A missing, duplicate or unsupported version is answered
   with RFC 6455's `426 Upgrade Required` and `Sec-WebSocket-Version: 13` so a
   client can retry. These protocol refusals reap and account the request and
   return `null`; callers should continue their accept loop. They do not throw a
   `ValueError` or leave a pending request to answer.
3. A valid handshake writes `101 Switching Protocols` to the connection's
   bufferevent, with `Sec-WebSocket-Accept` computed in C
   (`base64(sha1(key || GUID))`) and `$responseHeaders` appended after
   validation — pass `Sec-WebSocket-Protocol` there. Hop-by-hop headers are
   refused, the same list every response on this executor obeys.
4. The valid path hijacks the connection away from evhttp (the same three steps
   libevent 2.2's own `evws_new_session()` performs, reduced to the public 2.1
   API), wraps the bufferevent — the OpenSSL one on a TLS pool, whole, since
   the fd carries only ciphertext — in a `php_stream` and returns it. Other
   programming errors (unknown id, already-answered request, invalid response
   headers) still throw before the handshake changes state.

The returned resource is a normal stream: `fread()`, `fwrite()`, `fclose()`,
`feof()` work, and so do the existing primitives with no new API —
`fpmng_worker_event_create(FPMNG_WORKER_READ|WRITE, $stream, $cb)` for
readiness, `fpmng_worker_stream_has_buffered()` for the already-buffered case.
`fclose()` tears the connection down (fd, and the `SSL*` on TLS), and the
worker's own retirement closes it the same way.

Semantics worth knowing:

- **Readiness is not the raw fd.** The bufferevent drains the descriptor into
  its input buffer, where an fd watcher cannot see it — so the watcher bound
  to an upgraded stream is fired by the connection itself when frames arrive
  or the output buffer drains. It behaves like a plain read watcher; it just
  is not `select(2)` on the descriptor.
- **Read until short.** `fread()` returns everything buffered, `''` when
  there is none — an empty buffer is the normal state between frames, not
  EOF. `feof()` becomes true only when the peer (or the worker) actually
  closed. Read until `fread()` comes up short, the same rule every stream on
  this executor obeys.
- **Backpressure is #332's contract**: `fwrite()` returns `0` once the
  connection's queued-but-unwritten output reaches
  `worker.send_buffer_limit`, and the write watcher wakes the codec when it
  drains. There is no WebSocket-specific backpressure.
- **The hijacked connection is not a pending request**: `worker.max_pending`
  and `worker.request_timeout` do not bound it; it counts towards
  `pm.max_requests` like any answered request. `http.read_timeout` no longer
  applies to it either (the bufferevent timeout is cleared at the hijack) —
  liveness is the codec's ping/pong.
- **Framing is userland.** Masking, fragmentation, ping/pong, close codes are
  RFC 6455 byte manipulation, done by `amphp/websocket-server`,
  `ratchet/rfc6455` or a codec of your own (`examples/http-direct-worker-ws/`
  has a minimal one). On retirement the driver sees `fpmng_worker_stopping()`
  and sends its `1001 Going Away` before the loop exits — C guarantees only
  that the fd is closed, not that the protocol was.
- **The gateway cannot proxy any of this** (#344 answers `Upgrade` with 501):
  FastCGI has no way to carry a raw bidirectional stream. #68's three
  impossibility arguments stand for the classic executor and the gateway;
  what this section lifts is the third one, for the worker executor only —
  the one that runs PHP next to its own event loop.
- Because a hijacked connection never finishes its request, a WebSocket
  server's driver does not exit on `fpmng_worker_may_exit()` alone — the
  driver keeps its own connections until it has closed them (see the
  example).

## Connection limits (`http.max_connections`)


Three policies, added by issue #61, all of them **per worker**. A pool with
`pm.max_children = 4` and `http.max_connections = 64` allows up to 256
connections; there is no counter shared between the children, and there is not
meant to be one — a shared counter would need shared memory on the accept path,
and a child is what owns the fds.

```ini
http.read_timeout = 5000              ; ms — also the first-request deadline
http.max_connections = 64             ; per worker, 0 = unlimited (default)
http.max_connections_per_client = 8   ; per peer address, per worker, 0 = unlimited
```

Both limits are validated at startup (`php-fpm-ng -t`). Each must be between 0
and 1000000; a per-client cap above `http.max_connections` is refused because it
could never be reached; and a per-client cap **requires** `http.max_connections`.
The last one is not tidiness: the per-client count is a walk of the connections
this worker already holds, done once per accepted connection, so without a total
cap the accept path would grow with the flood the directive exists to survive.

### The first-request deadline

`http.read_timeout` is documented as one budget for the whole client-side read.
On the http gateway it is exactly that; on a direct pool it used to be only
libevent's idle timeout, which every arriving byte resets.

Measured on the poligon, 2026-09-11, against a direct pool with
`http.read_timeout = 3000`:

| client | before issue #61 |
| --- | --- |
| sends nothing at all | dropped at 3.0 s |
| sends one byte of the request line every 2 s | held the connection **56 s**, then was served normally |

So only the trickle was unbounded — which is the shape of slowloris. Since
issue #61 a connection whose **first** request has not fully arrived within
`http.read_timeout` is closed, whatever it has been dripping. The deadline is
armed on accept and disarmed the moment the first request is complete; libevent
2.1.12 offers no request-start hook, so later requests on the same keep-alive
connection are covered by the idle timeout only. The first time a child actually drops a
connection this way it says so once, at `NOTICE`; there is no line while nothing
is being dropped, and the line needs `catch_workers_output = yes` to reach the
error log at all (issue #73). `http.max_connections_per_client` announces itself
the same way, once per child, the first time it refuses somebody.

### The two limits, and the two shapes of refusal

A worker at `http.max_connections` **stops accepting** instead of refusing: the
listening socket is shared by every child of the pool, so a connection left in
the queue is one a sibling can take, while a connection refused with a response
is one nobody can. This reuses the accept gate from issue #53. The worker
resumes accepting as soon as one of its connections goes away.

`http.max_connections_per_client` cannot work that way — whether a connection
is over the cap depends on who it is from, which is not known until it has been
accepted. That one is enforced per connection, and takes either of two shapes,
both of which a client must be prepared for:

- the connection is **closed without a response**, when the cap is passed
  before evhttp has handed us a request;
- the request is answered **`503 Too many connections`**, when it is passed
  inside the request callback. (Forcing a drop from there is not possible:
  `EV_READ` is disabled on the bufferevent for the duration of the callback, so
  the zero-timeout trick that drops a connection from outside does nothing.)

The 503 is counted and logged exactly like the pool's other refusals — it
shows up on `pm.status` as `refused connections` and produces an `access.log`
line — and it is emitted **after** `listen.allowed_clients`, so an address the
ACL excludes still gets a plain `403 Forbidden` and learns nothing about the
cap. It has its own counter rather than joining `refused requests`, because a
connection refused before its first request is not a request the pool answered;
connections dropped by the deadline are `timed out connections` for the same
reason.

The suite (`sapi/fpmng/tests/fpmng-http-direct-connection-limits.phpt`) accepts
either shape for exactly this reason.

### Under `pool.executor = worker`

The deadline applies. The two limits are **rejected by `-t`**, not silently
ignored: enforcing a total limit means not accepting, and the accept gate is
part of the classic executor's loop, which this executor deliberately does not
have (it drives the loop from userland with several requests in flight). What
bounds a worker-executor child instead is the 256 held requests described in
[Held requests](#held-requests-under-poolexecutor--worker-decision-2026-09-11).

### What this is not: a throughput defence (measured, 2026-09-11)

The limits are a **resource ceiling**, not a way to keep a worker fast under
connection pressure. Measured on the poligon (4 children, 32 legit source
addresses, against 200 idle connection hoarders):

| configuration | 0 hoarders | 200 hoarders |
| --- | --- | --- |
| no limit | 5859.5 rps | 5837.4 rps |
| `http.max_connections = 64` | 5723.1 rps | 5710.4 rps |
| `http.max_connections_per_client = 5` | 5741.8 rps | 5653.0 rps |

(The per-client row was measured before the total cap became mandatory
alongside it; the per-client walk it times is the same one.)

Idle connections cost this worker nothing measurable, so capping them buys
nothing measurable either — and the tracking costs nothing measurable in
return. The spread above is within the run-to-run noise of the harness.

An earlier run of the same harness appeared to show a limit costing 1738 → 372
rps. It was an artefact: the control that runs the configurations in the
reverse order reproduced the collapse on the *unlimited* pool, and the cause
was TIME_WAIT / ephemeral-port exhaustion on the loopback client, not anything
in the pool. It is recorded here so the number is not measured again and
believed.

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
- Nothing streams that cannot: an **HTTP/1.0 client** (there is no chunked
  framing to use, and a `Content-Length: 0` followed by a body would let a
  keep-alive client read that body as the next response), a response for which
  the script **declared its own `Content-Length`** (libevent only chooses
  chunked framing when there is none, so streaming past that point would write a
  body nobody checked against the declared length), a non-final status
  (`1xx`), a bodyless response (HEAD, 204, 205, 304), or a response already
  doomed by the header caps. All of these stay on the buffered path, so the
  existing framing and error behaviour is unchanged. The task 054 tests measure
  the default and are unmodified.
- `pm.max_requests` and a pending shutdown add `Connection: close` before the
  headers go out. A SIGQUIT that arrives *after* that point cannot: the header
  is already on the wire, so a client on that one connection learns the child
  is retiring from the close rather than from the header. The buffered path,
  which decides with the whole response in hand, is not affected.

**Failure mode when the client stalls.** `http.stream_write_timeout` is the
total time the worker may spend *blocked* on the client across one response —
not a per-write budget, which a client taking one byte before each deadline
would renew forever. A client keeping up with the response never spends any of
it. On expiry — or on any write error, or if the peer closes — the worker logs
a `WARNING` (`the client stopped reading the response`), drops the remainder and
shuts the socket down **without the terminating chunk**. The client therefore
sees an unterminated chunked message and can tell the response is incomplete; it
never sees a silently short 200. The worker's memory does not grow after the
abort: output from the still-running script is discarded, and the script runs to
its normal shutdown.

**Over TLS too, since issue #195.** The combination used to be refused at
startup: writing from inside a running request means not re-entering the event
loop — libevent refuses a reentrant `event_base_loop()` on the base it is already
dispatching from — so the writer drove the connection's own descriptor, which is
only correct while the descriptor and the bufferevent carry the same bytes, and
on a TLS connection they do not. libevent 2.1.12 offers no way out of its own:
`be_openssl_flush()` is an unimplemented stub (`bufferevent_openssl.c:1259`) and
`bufferevent_base_set()` refuses a non-socket bufferevent.

What the pool does instead is what libevent's own writer does, on our stack: the
write step is a function pointer chosen once per child, and on a TLS pool it
peeks the bufferevent's output buffer, hands the peeked bytes to `SSL_write()`
and drains what OpenSSL took — the same three calls as `do_write()`
(`bufferevent_openssl.c:654`), minus the loop. Two properties of libevent make
that safe rather than a second writer racing the first: the pool uses
`bufferevent_openssl_socket_new()`, and in socket mode (no underlying
bufferevent) libevent never writes from the buffer callback, so nothing can be
in flight while the request callback holds the loop; and libevent sets
`SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER` (`bufferevent_openssl.c:1369`), which
makes retrying a blocked write from a re-peeked buffer legal as long as the
retry is at least as long as the one that blocked — so the pool hands OpenSSL
the peeked vector's whole length, the same length `do_write()` would pass, and
never a clamped one. Unlike the
plaintext step, the TLS one must *not* freeze the output buffer around the
write: an OpenSSL bufferevent never freezes it (the freeze belongs to
`bufferevent_socket_new()`, `bufferevent_sock.c:373`), and leaving it frozen
makes libevent's own `evbuffer_drain()` fail while `SSL_write()` keeps
succeeding — measured as the terminating chunk written ~542,000 times in a tight
loop after an otherwise correct 16 MiB body.

### Measured results (Intel i7-6700T, 8 threads, poligon, 2026-09-11)

`build/benchmark-http-direct-stream.py`, php-8.5.9, one static worker per pool,
`output_buffering = 0`, loopback client, median of 15 rounds. Peak RSS is the
pool's largest resident size sampled from `/proc` every 5 ms while the response
is in flight. Raw data: `metadata.json` / `results.json` in the scratch folder.

| response | pool | status | TTFB | total | peak RSS |
| --- | --- | --- | --- | --- | --- |
| 4 MiB | buffered | 200 | 4.83 ms | 8.28 ms | 29.7 MiB |
| 4 MiB | streamed | 200 | **0.27 ms** | 2.13 ms | **25.3 MiB** |
| 16 MiB | buffered | 500 (over the 8 MiB cap) | 5.94 ms | 5.94 ms | 33.6 MiB |
| 16 MiB | streamed | 200 | **0.22 ms** | 5.68 ms | **25.3 MiB** |

Time to first byte is ~18x lower at 4 MiB and does not grow with the response,
which is the point: it is the time to the first chunk, not to the last. Peak RSS
is flat across both sizes for the streamed pool and grows with the response for
the buffered one; at 16 MiB the buffered pool has no answer at all. The
loopback client never stalls, so these numbers do not exercise backpressure —
`sapi/fpmng/tests/fpmng-http-direct-streaming.phpt` does.

## Finishing the response early (`fpmng_respond()`)

Available on `pool.type = http-direct` pools, registered per worker, no
configuration. It finishes the current response — status, headers and everything
written so far — and lets the script keep running:

```php
<?php
echo json_encode(['ok' => true]);
fpmng_respond();

// The client already has the response. This runs after it.
$queue->push($job);
$log->write($expensiveSummary);
```

Returns `true` when it finished the response, `false` when there was nothing to
finish: no request in flight, the client is already gone, or the response was
finished by an earlier call. A second call is therefore safe and does nothing.

**What it gives you.** The client has the complete, correctly framed response
while the script is still running. It is the http-direct counterpart of
`fastcgi_finish_request()` on a FastCGI pool, and it moves the request to the
`Finished` stage in the scoreboard, so a script that keeps working past this
point falls under `request_terminate_timeout` only when
`request_terminate_timeout_track_finished` is on — the same rule a FastCGI pool
applies.

**What it does not give you.** The worker is *not* free. It serves no other
connection until the script actually ends, on any pool type. That is a property
of the classic executor: `php_execute_script()` runs inline inside the event
loop callback, so the loop is blocked for as long as the script runs. This
function shortens the *client's* wait, not the worker's occupancy — if the aim
is to free the worker, the work belongs in a queue consumed elsewhere, not after
`fpmng_respond()`.

Because the event loop is blocked, queueing the finished response with libevent
would not put a single byte on the wire until the script returned. So the call
writes the finished response to the connection itself, which is what makes the
early delivery real rather than bookkeeping.

**On a TLS pool it does the same thing** (issue #195): the early write goes
through the same per-child write step as streaming does, so the finished
response is encrypted and put on the wire from inside the call rather than left
for the event loop. Before #195 the direct write was refused on such a pool and
the call moved only the accounting.

`fastcgi_finish_request()` remains disabled on http-direct pools rather than
being pointed at this function; that is a separate decision about framework
compatibility, not an oversight.

Measured on the test box (`sapi/fpmng/tests/fpmng-http-direct-respond.phpt`,
script sleeping 1500 ms after the call): the client has the whole response in
well under 700 ms on both a buffered and a streaming pool. Without the direct
write the same test measures 1501 ms — the full length of the sleep.

## Static files (`http.static`)

A direct pool can answer `GET` and `HEAD` for files under its document root
itself, without starting a PHP request at all:

```ini
[app]
pool.type = http-direct
chdir = /srv/app/public
http.front_controller = /index.php
http.static = yes
```

The document root is `chdir` — the same directory `DOCUMENT_ROOT` reports to
the script. There is no separate root directive: a direct pool already has
exactly one, validated at startup together with the front controller, and a
second one would only be a second thing to get out of step.

`http.static` is **off by default on a direct pool**, and this is the one place
it differs from the `http` gateway, where the same directive defaults to on.
The gateway's document root is a document root because someone chose it as one.
A direct pool's is wherever the application lives, next to `vendor/`, config
and whatever else the framework keeps beside its front controller — so a pool
that never asked for a file server must not get one on upgrade.

### What is served

| | |
|---|---|
| Methods | `GET` and `HEAD`. Everything else goes to PHP. |
| Files | Regular files with an extension the built-in table has a type for. |
| Headers | `Content-Type`, `Content-Length`, `ETag`, `Last-Modified`. |
| Conditional | `If-None-Match` and `If-Modified-Since` (exact IMF-fixdate match) answer `304`. |
| Delivery | `evbuffer_add_file()`, i.e. `sendfile()`/`mmap` where the platform has it: the bytes never pass through the process. |

Anything else is handed to the front controller exactly as it would have been
with `http.static = no`: a directory, a path ending in `.php` or containing
`.php/`, a file that is not there, and — deliberately — a file whose extension
the table has no type for. On the gateway an unknown extension is served as
`application/octet-stream`; on a direct pool it is left alone, because there the
root is an application directory and "I do not know what this is" is a reason
not to hand the file over.

### What is refused

Refused with a `404`, never handed on:

- any path with a dot-segment in it (`/.env.css`, `/assets/.hidden/app.css`) —
  this is the default here, not a rule an operator has to write;
- anything that resolves, through `realpath()`, outside the document root —
  including by symlink, which a textual `..` filter does not catch. The refusal
  is logged at `NOTICE` with the pool name and the requested path.

A refusal is a plain `404` that leaves the connection open. The next request on
it is as valid as this one was, and one probe for `/.env.css` must not tear down
the connection carrying the rest of a page's assets.

Percent-encoded traversal (`/%2e%2e/%2e%2e/etc/passwd`) never reaches the
filesystem: the path is decoded first and then checked, so `%2e%2e` is `..` by
the time the rule looks at it.

The dot-segment rule applies where this module would otherwise have **served**
the file. `/.env` has no known extension, so it is still the application's to
route — it was before `http.static` was turned on, and enabling a file server
must not make a URL disappear. `/.env.css` would have been served, so it is
refused. Either way no file whose path contains a dot-segment ever leaves the
static path.

### What it deliberately does not do

- **No compression.** No gzip, no brotli, no `Accept-Encoding` negotiation.
- **No range requests.** No `Accept-Ranges`, no `206`; a `Range` header is
  ignored and the whole file is sent.
- **No cache policy.** No `Cache-Control`, no `Expires`, no `max-age`. The
  validators above are all the caching this offers; an origin that wants a
  policy puts a CDN or a reverse proxy in front, or serves the asset from PHP.
- **No index file.** A directory is not "the directory's `index.html`", it is
  a request for the front controller to route.
- **No directory listing**, in any configuration.
- **Only on the classic executor.** `pool.type = http-direct` with
  `pool.executor = worker` rejects `http.static` at startup rather than
  accepting a directive nothing there would honour.

### Measured results (Intel i7-6700T, 8 threads, poligon, 2026-09-11)

`build/benchmark-http-direct-static.py`, three rounds per cell in rotated order,
2 s of warm-up and 8 s measured. Three backends serve a byte-identical file at
the same URL: this path, the same pool with `http.static = no` and a front
controller `readfile()`ing the file, and nginx 1.28.3 from disk. Server CPU is
read from `/proc` for our own process trees only, so the comparison is of
servers and not of the machine. Medians of three.

| bytes | conc | backend | rps | CPU µs/response | CPU ns/byte | p99 ms |
| --- | --- | --- | --- | --- | --- | --- |
| 4096 | 1 | `direct-static` | 14 398 | 65.3 | 15.93 | 0.12 |
| 4096 | 1 | `direct-php` | 6 080 | 140.6 | 34.33 | 0.26 |
| 4096 | 1 | nginx | 22 869 | 34.1 | 8.32 | 0.08 |
| 4096 | 32 | `direct-static` | 67 259 | 59.4 | 14.50 | 0.85 |
| 4096 | 32 | `direct-php` | 29 099 | 137.4 | 33.54 | 2.11 |
| 4096 | 32 | nginx | 115 150 | 34.4 | 8.40 | 0.44 |
| 262 144 | 1 | `direct-static` | 4 252 | 234.5 | 0.89 | 0.36 |
| 262 144 | 1 | `direct-php` | 2 892 | 326.2 | 1.24 | 0.50 |
| 262 144 | 1 | nginx | 5 182 | 65.2 | 0.25 | 0.31 |
| 262 144 | 32 | `direct-static` | 13 560 | 273.8 | 1.04 | 3.21 |
| 262 144 | 32 | `direct-php` | 7 413 | 519.7 | 1.98 | 6.18 |
| 262 144 | 32 | nginx | 13 862 | 62.9 | 0.24 | 2.87 |

Against the front controller, which is where these bytes go today: 2.2x to 2.3x
less CPU per byte at 4 KiB and 1.4x to 1.9x at 256 KiB. Against nginx: 1.7x to
1.9x more at 4 KiB and 3.6x to 4.4x at 256 KiB, though on throughput at 256 KiB
and concurrency 32 the two are within 2 % of each other, because there the run
is bounded by the loopback and not by either server.

The small file is the honest case for the per-request overhead and the large one
for the delivery: at 4 KiB almost all of the 65 µs is libevent's request
handling, and the file itself is one `evbuffer_add_file()` the kernel satisfies
without the bytes passing through this process. That is the whole reason the
per-byte figure falls from 15.93 to 0.89 ns as the file grows sixty-four fold.

The first run of this benchmark measured something else entirely. At 256 KiB
every direct arm reported 31 rps with a p99 of 43 ms, which is not a server but
the peer's delayed-ACK timer -- the pool's listening socket did not have
`TCP_NODELAY` (issue #244). The numbers above were taken after that was fixed;
the ones in #244 are the two sides of it.

### Accounting

A static hit costs no PHP request, and is not counted as one: it does not
advance `pm.max_requests`, and it does not move the scoreboard. It does hold a
slot in the pool's in-flight count until the client has taken the response or
gone away, which is what stops a child exiting with a reply still on the wire.
It is counted separately as a `non-php request` on the status page and written
to `access.log` like any other response — see
[Operating a direct pool](#operating-a-direct-pool).

The implementation is shared with the `http` gateway
(`sapi/fpmng/fpm/fpm_http_static.c`) rather than copied: the containment check,
the dot-segment rule and the conditional handling are the parts that have to be
right, and a second copy of them is a second place to get them wrong. The
gateway gained `Last-Modified` and `If-Modified-Since` from the same change.

## Operating a direct pool

Since issue #59 a direct pool answers the pool-level operator directives an
FPM operator already knows, on the **classic executor**:

```ini
[app]
pool.type = http-direct
listen = 127.0.0.1:9100
chdir = /srv/app/public
http.front_controller = /index.php

ping.path = /ping
ping.response = pong
operator.status_path = /status
operator.status_listen = 127.0.0.1:9253
access.log = /var/log/php-fpm/app.access.log
access.format = "%R - %u %t \"%m %r%Q%q\" %s %{milli}d %{kilo}M"
access.suppress_path[] = /ping
listen.allowed_clients = 10.0.0.4,10.0.0.5
chroot = /srv/jail
```

### `ping.path` and `operator.status_path`

**They are on two different sockets.**

`ping.path` is answered by the worker's own event loop on the pool's listener,
before any PHP request is started, and does not count against
`pm.max_requests`. It is a liveness probe for whatever is in front of the pool,
so that is where it belongs.

`operator.status_path` is answered by the pool's **operator endpoint**, on
`operator.status_listen` (default `127.0.0.1:9253`) — see
[`docs/operator-endpoint.md`](operator-endpoint.md). It used to be on the pool's
own listener; issue #275 moved it, unchanged. The page, its fields, its two
flags and its headers are exactly what they were; what changed is that the
scrape is no longer one of the pool's own requests, so it is in none of the
counters below and in no `access.log`. On the pool's own listener the path is
the application's again, like any other URL.

Both are matched against the request path with the query string cut off, and
matched *whole*: `/statuses` is not `/status`. There is no percent-decoding —
the directive is a literal in the pool file and upstream matches it literally
too, so `/%73tatus` is not a way past a proxy rule written against the
documented spelling.

`operator.status_path` answers plain text, or JSON for `?json`, with the same
`Expires`/`Cache-Control` headers upstream's `fpm_status.c` sends. The fields:

| Field | Where it comes from |
|---|---|
| `pool`, `process manager`, `start time`, `start since` | the pool's scoreboard |
| `idle processes`, `active processes`, `total processes`, `max active processes`, `max children reached` | the pool's scoreboard |
| `requests`, `slow requests`, `memory peak` | the pool's scoreboard — PHP requests only |
| `accepted conn` | connections this pool's children accepted, counted in the one hook libevent runs per accepted connection |
| `non-php requests` | static files and pings: answered without starting a PHP request |
| `refused requests` | answered `403` by `listen.allowed_clients`, or `503` because the pool was stopping or saturated |
| `active requests` | PHP requests in flight right now |
| `direct schema` | the version of the direct-specific fields below (currently `1`) |
| `live connections` | connections this pool's children hold right now |
| `pending responses` | responses accepted but not yet fully written |
| `refused acl` | requests answered `403` by `listen.allowed_clients` |
| `refused capacity` | requests answered `503` because the pool was stopping or saturated |
| `refused connections` | connections answered `503` by `http.max_connections_per_client` |
| `timed out connections` | connections dropped by the first-request deadline |
| `rejected responses` | responses PHP produced that could not be written to the client |
| `retiring children` | children draining towards their own exit (see below) |

`refused requests` is the sum of `refused acl` and `refused capacity`, kept
under its old name and its old meaning so a tool written against the fastcgi
page keeps working. The split is what the sum could never answer: "the pool is
refusing" and "the pool is refusing *the people you told it to refuse*" are
different incidents.

`direct schema` exists so that a tool meeting a page it does not understand can
say so instead of guessing from which fields happen to be present. It is
bumped when a field changes meaning or leaves; adding a field does not bump it.

#### What tracking a connection costs

Reporting `live connections` means keeping a small node per connection for as
long as the connection lasts, and releasing it when the connection ends. That
release is a sweep, and the sweep is not free: `bufferevent_getcb()` -- the
call that asks libevent whether evhttp has let go -- takes the bufferevent's
lock.

Measured on the poligon 2026-09-12, one child, no `http.max_connections`, one
busy keep-alive connection against N idle ones:

| N idle connections | origin/main | exhaustive sweep per tick | bounded sweep (shipped) |
|---:|---:|---:|---:|
| 0 | 9534 rps | 9460 rps | 9320 rps |
| 5 | -- | -- | 9778 rps |
| 32 | -- | -- | 9272 rps |
| 500 | 9555 rps | 9180 rps | 9426 rps |
| 2000 | 9686 rps | 6708 rps | 9369 rps |

The first two columns are the run that decided the design; the third is a
re-measurement of the binary that shipped, so read it for its flatness rather
than against the other two. It has no trend: the two `N = 0` measurements of
that run were 9267 and 9320 rps, which is the width of the noise.

So the sweep examines at most 32 nodes per pass, taken from the end of the list
that requests move away from. A descriptor whose connection has ended is
therefore released within `live / 32` ticks rather than one, which for 2000
connections is under a second -- measured as `live connections` falling from
1408 to 0 within three seconds of the client closing them all. Anything that
needs an exact count right now (the accept gate at `http.max_connections`, the
per-client count) asks for an exhaustive walk instead, and both of those only
exist when a cap is configured, which is what bounds the walk.

#### What each number costs to read

Three of these are **gauges**, not totals: `live connections`, `pending
responses`, and — on `?full` — the same two per child. They are published into
the shared slot from the worker's 10 ms tick, so they are up to 10 ms stale,
and they stop moving entirely while that child is inside PHP: on the classic
executor the script runs inside evhttp's request callback, so the loop that
would publish them is not running. A child stuck in a 5 s script reports the
connection count it had when the script started. This is deliberate — the
alternative is a shared-memory write on every accept and every close, on the
path the direct pool exists to keep short.

`active requests` is the exception, and it is why the distinction matters: it
is written on the request path itself, so it *does* move while PHP runs. That
is what makes "this child is busy" visible at all.

#### Reset semantics

`accepted conn`, `non-php requests`, `requests`, `refused *` and `rejected
responses` belong to the pool, not to a child: a child recycled by
`pm.max_requests` inherits the counters of the slot it takes over, so the
totals never go backwards mid-series. The gauges (`active requests`, `live
connections`, `pending responses`) are cleared when a child starts, because
that is exactly what a child killed mid-request would otherwise leak.

`timed out connections` and `refused connections` are pool totals like the
rest, even though the numbers behind them live in each child's own memory: the
tick publishes the *difference* since its last publish, so a child adds to the
slot it inherits instead of overwriting it. Assigning instead would have made
both rows drop to zero at every `pm.max_requests` recycle, and a scraper reads
a counter that drops as a reset of the whole series.

The gauges are summed over the **live** children only. A slot belongs to a
scoreboard index and is only ever re-zeroed by the next child to take that
index, so a child that was scaled down, recycled or killed outright would
otherwise leave `live connections: 20` standing for as long as the pool stayed
small. The page checks the scoreboard's `used` flag for each slot instead
(`fpm_http_direct_ops.c`, `fpm_http_direct_ops_slot_alive()`), which needs
nothing to run in the dying child and therefore also covers `SIGKILL`,
`request_terminate_timeout` and a crash. On `?full` the same fact is a row:
`live: 1` or `live: 0` says whether a child holds the slot at all, which is
what tells "this child holds no connections" apart from "no child here".

A reload (`SIGUSR2`) starts every number here again, the scoreboard's own
`requests` included: the master re-executes, so the shared segment is a new
one. Measured on the poligon 2026-09-11 -- `accepted conn` 9 and `requests` 24
before, `accepted conn` 1 (the status request itself) and `requests` 0 after.
The re-entrancy guard in `fpm_http_direct_ops_init_main()`
(`fpm_http_direct_ops.c:88`) is about the per-pool init being re-run inside one
master process, not about surviving a reload. A monitoring series therefore has
to treat a reload as a counter reset, which is what tooling already does for
upstream's `accepted conn`.

#### `?full`: the per-child rows

Every row also carries `pid` and `retiring`, which the pool-wide block has no
place for: the pid because it is the address of the retire signal below, and
`retiring` because it is a property of one child.

`?full` appends one block per scoreboard slot, with the same fields, in both
the text and the JSON rendering (`?json&full` stays valid JSON: the per-child
rows are a `workers` array). A slot that has accepted nothing is printed
anyway, because "this child got none" is the observation.

This is what makes the accept distribution measurable from the status page
alone. The fairness problem of issue #53 — one child accepting a whole
keep-alive burst while the others sat idle — needed an external harness to see;
a pool-wide sum cannot show it at all. Two children whose `accepted conn` reads
1000 and 4 is the same measurement, from the page an operator already has open.

Measured on the poligon 2026-09-11 (4 children, 64 keep-alive connections x 20
requests, then one `?full` read), the accept distribution came off the page as
`accepted conn` 19 / 22 / 22 / 2 across the four slots -- the same observation
issue #53 needed a separate harness to make.

That same read is a worked example of the staleness above: it reported `live
connections: 57` a moment after the client had closed all 64, because the
children had not ticked yet. Three seconds later the same page read `0`, and
the children's open descriptors were back to 10 each. The gauge is a gauge.

Upstream's fastcgi `?full` reports a per-process *request* detail (the URI, the
method, the duration). That part is still absent: the scoreboard's per-process
slots describe a FastCGI request, and a direct child's request is not one.

`operator.status_listen` names where the page is served since issue #275. It was
rejected before that, when it could only have asked for a second FastCGI socket
a direct child has nowhere to put. On `pool.executor = worker` the page is not
served at all, because `operator.status_path` is rejected there for the reason below.

The page is rendered by a process that is not one of this pool's children, which
is why every number on it comes from shared memory: the per-slot counters the
master allocates before the first fork, and the pool's scoreboard. Reading
another process's counters is what any operator page does; it is why the page
can be rendered at all by something that is not a child of this pool.

### Retiring one child (`SIGUSR1`)

`kill -USR1 <child pid>` tells one child of a direct pool to step out: it stops
accepting, finishes what it is doing, and exits, and the master puts a
replacement in its place the way it does for any child that exits on its own.
The pid is on the `?full` row of the status page, which is the point of it
being there.

This is what a rolling deploy needs and what `SIGQUIT` cannot give. `SIGQUIT`
is the pool winding down: a child that has it answers `503` to everything that
arrives, which is right when the whole pool is going away and wrong when the
pool is carrying on without this one child. A retiring child keeps serving:

- it removes itself from the pool's listening socket immediately, so every
  connection it does not take is one a sibling takes -- the socket is shared,
  so nothing is refused and nothing is queued behind the child that is leaving;
- it answers the requests that arrive on the connections it already holds, and
  every answer carries `Connection: close`, so a keep-alive client is told to
  go and open its next connection somewhere else rather than finding out from a
  reset;
- it exits when it holds no connection and no response is still being written,
  or when `http.read_timeout` has passed since the signal -- whichever is
  first, and the deadline wins over work still in flight. Nothing else bounds
  a retiring child: a pool-wide stop is bounded by the master, which kills
  whatever has not gone by `process_control_timeout`, but the master is not
  waiting for this one. Past the deadline a client that has not finished
  reading its response gets it truncated -- the alternative is a client reading
  a byte a second holding the child, and the deploy, open indefinitely.
  `http.read_timeout` rather than a knob of its own because it is already this
  pool's answer to "how long may a connection stay silent", so a pool that has
  tuned one has tuned both; a pool that serves large responses to slow clients
  should size it for the download, not for the silence.

A second `SIGUSR1` to a child that is already retiring does nothing: the drain
it is waiting for is not shortened by asking twice.

Retiring every child one at a time is a rolling restart of the pool with no
failed request, which is what the test asserts, with a client that opens a
connection per request throughout. One at a time is the contract: the siblings
are what keeps the pool answering, so wait for the replacement -- `?full` shows
it as a new `pid` in that slot -- before signalling the next.

Interactions:

- **`pm.max_requests`.** Both end in the same drain. A child that reaches its
  recycling limit while retiring simply has two reasons to go.
- **A graceful reload or stop.** The master's `SIGQUIT` wins: a child caught by
  a pool-wide shutdown follows the shutdown, `503` included. There is no point
  serving new requests for a pool that is going away.
- **`http.max_connections`.** A retiring child stops accepting, so it cannot
  reach the cap; the connections it already holds count against nothing else.
- **`pool.executor = worker`.** `SIGUSR1` there is the same graceful stop
  `SIGQUIT` gives. That executor tracks no connections, so it has nothing with
  which to tell "drain the connections I hold" from "drain the requests I
  hold". What it does buy is that the signal is not the default action, which
  would kill the child outright and drop what it was serving.

What this deliberately does **not** do is start the replacement before the old
child exits. The replacement arrives when the slot frees, so a pool of `N`
children serves on `N - 1` for the length of one drain. Starting it earlier
would mean running `pm.max_children + 1` processes, and `pm.max_children` is
the promise on which an operator sized the machine's memory. Retire one child
at a time and the pool never dips below `N - 1`.

### `access.log`

The format is the FastCGI one, validated by the same code that validates it for
a fastcgi pool (`fpm_conf.c` runs every pool's `access.format` through
upstream's parser at startup), and the file is the descriptor the master opened
before the first fork — so `SIGUSR1` rotation works exactly as it does
elsewhere. One `write()` per line on an `O_APPEND` descriptor is what keeps the
children's lines from interleaving.

The renderer is ours (`fpm_http_direct_access_log.c`) rather than upstream's
`fpm_log.c`, for a reason that is not stylistic: `%e{VAR}` there casts
`SG(server_context)` to a `fcgi_request *`, which under this transport is a
`struct fpm_direct_request *` — reading a foreign object — and `%R` reads a
`fastcgi.c` global that nothing in a direct pool writes. Everything else reads
the scoreboard slot and is portable, and that is what is reused.

What differs from a fastcgi pool:

- `%e{VAR}` reads the CGI environment this pool built for the request, and `%R`
  is the direct peer address (never an `X-Forwarded-For`: a direct pool has no
  trusted-proxy list).
- `%r` is the request path. On a fastcgi pool it is `SCRIPT_NAME`, which for a
  front-controller application is always `/index.php`; here the path is what
  the client asked for, and `%Q%q` still carries the query string exactly once.
- Responses that never ran PHP — a static file, a ping, a `403` or a `503` —
  are logged too, with the fields that do not apply (`%M`, `%C`, `%f`, `%u`)
  left at zero or `-` rather than carried over from whatever this child served
  last. Scrapes of `operator.status_path` are not among them: since issue #275 they
  never reach this pool.

- `access.suppress_path[]` matches the same request path.

Under `pool.executor = worker`, `operator.status_path`/`operator.status` and
`access.*` are **rejected**, for the same reason `request_terminate_timeout` is:
that executor calls `fpm_request_accepting(false)` once for the life of the
child, so there is no per-request stage, duration, CPU or peak memory to
report. Refusing the directive is better than answering it with placeholders.
Issue #387 decided the status page **stays refused** here rather than being
reduced to a second, differently-shaped page — which is exactly what #275
avoided when it moved http-direct's page unchanged. `operator.metrics_path` is
the honest view of this executor.

`ping.path`/`ping.response` are **supported** since issue #387. Ping needs none
of that per-request accounting — it is a literal path match in the connection
handler — and this executor has a request listener and serves requests, so
refusing it was the one place `docs/gateway.md`'s first rule ("ping is answered
on the request listener, by a process that serves requests there") did not
hold. It is answered after the saturation `503` and ahead of the userland
queue, so it touches neither the scoreboard's per-request accounting nor
`worker.max_pending`.

That does not make `operator.metrics_path` (which this executor does not reject)
untruthful. Since issue #333 its `requests` total is a real count, incremented
once per request this executor actually answers — both through the buffered
`fpmng_worker_respond()` and through the streaming completion of
`fpmng_worker_respond_end()` (issue #332) — rather than the field it used to
report by never touching it at all. Two gauges sit alongside it on the same
endpoint: `fpmng_pool_worker_pending` (requests accepted but not yet answered,
mid-handler or mid-stream) and `fpmng_pool_worker_watchers` (libevent watchers
this pool's workers currently have registered via
`fpmng_worker_event_create()`). Both are sums across every child of the pool,
published synchronously on every change rather than on a tick, and both drop
back down — pending on every `fpm_worker_reap()` path (answered, timed out by
`worker.request_timeout`, or the connection going away), watchers on
`fpmng_worker_event_free()` — so neither one only grows. What `operator.metrics_path`
still cannot say for this executor is the same thing `operator.status_path` cannot:
idle vs. active per request, a request's duration, or its CPU/peak memory —
the scoreboard's `idle`/`active` pair for a worker pool therefore keeps reading
`idle=N, active=0` regardless of how many requests are actually in flight,
because that pair is written from `fpm_request_accepting()`, called once per
child rather than once per request.

### `listen.allowed_clients`

A comma-separated list of literal IPv4/IPv6 addresses, the same matching rules
FastCGI's `listen.allowed_clients` uses (no CIDR; an IPv4 peer also matches an
allowed IPv4-mapped IPv6 entry). It shares the matcher with the gateway's
`http.allowed_clients` (`fpm_http_acl.c`).

It is enforced in the request callback, not at accept: libevent's
`evhttp_set_bevcb()` runs before the peer address is known. The TCP connection
is therefore accepted and the request answered `403`, which is also what the
`http` gateway does. A refused request reaches neither the static file server
nor PHP, and is counted as `refused acl` (and inside the
`refused requests` sum). A malformed address in the list is rejected by
`php-fpm -t` and at start-up,
before any child forks; it is fatal again in the child, which would otherwise
be the only place it was noticed. A list meant to keep someone out must never
end up keeping nobody out.

Both executors enforce it.

### `chroot`

`chroot` is the master's, applied by `fpm_unix_init_child()` before the child
reaches any of this code, so it works on both executors and needs nothing from
the transport. What issue #59 had to fix was the startup check: the master is
not chrooted, so the pool's `chdir` names a directory that only exists after
`chroot(2)`. The front controller is now resolved against `chroot + chdir` at
startup — the same prefixing `fpm_conf.c` already does when it checks that the
`chdir` exists — and against the plain `chdir` in the child, which runs after
the `chroot(2)`.

Because `chroot(2)` needs root, there is no `.phpt` that starts such a pool;
what is covered is the validation path, in
`sapi/fpmng/tests/fpmng-http-direct-config.phpt`.

## TLS

A direct pool terminates TLS itself, on both executors, using the same
implementation the `http` gateway uses (`fpm_tls_http.c`,
`fpm_tls_reload.c`) — no second TLS stack (issue #55).

**It has to be built in**: `./configure --enable-fpmng --enable-fpmng-tls`,
which the shipped packages are not (issue #280, see
[`tls.md`](tls.md#the-build-flag)). Without the flag a pool with
`http.tls_cert` is refused at startup, naming the flag; it is never served
cleartext.

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

Not covered here: OCSP stapling, session-ticket rotation beyond the shared key
generated at startup, and ACME issuance/renewal (issue #46). Client-certificate
verification and exposing connection facts to PHP are covered below.

Covered by `sapi/fpmng/tests/fpmng-http-direct-tls.phpt`: a handshake and a
request on each executor, the CGI variables, plain HTTP refused on the TLS
port, a reload picked up by new connections while one open across the swap
keeps working, and the master not restarting.

## Client certificate verification (`http.tls_verify_client`)

```ini
[app]
listen = 0.0.0.0:8443
pool.type = http-direct
http.tls_cert = /etc/ssl/app/fullchain.pem
http.tls_key = /etc/ssl/app/privkey.pem
http.tls_verify_client = optional     ; none (default) | optional | require
http.tls_client_ca = /etc/ssl/app/client-ca.pem
```

- `none` (default): no `CertificateRequest` is sent; the handshake never asks
  the client for a certificate.
- `optional`: the client is asked for a certificate but the handshake still
  completes if none is presented, or if the presented one does not chain to
  `http.tls_client_ca` — `fpm_connection_info()` (below) is how a script finds
  out which case it got.
- `require`: the handshake fails outright — no request is ever delivered, not
  even to `fpm_connection_info()` — unless a certificate chaining to
  `http.tls_client_ca` is presented. This is OpenSSL's own default verification
  (`SSL_VERIFY_PEER`, plus `SSL_VERIFY_FAIL_IF_NO_PEER_CERT` for `require`),
  with no custom callback: an untrusted or self-signed client certificate is
  rejected during the handshake itself, by OpenSSL, before any FPM-level code
  runs.

`http.tls_client_ca` is required whenever `http.tls_verify_client` is not
`none`; a pool with one and not the other fails to start, naming the problem.

## `fpm_connection_info()` (issue #62)

A direct pool's worker holds the real socket, so it can report connection
facts a FastCGI intermediary can only ever forward secondhand (or not at
all): the `http`/`fastcgi` gateway pools do not change what they put in
`$_SERVER` for this reason (out of scope for issue #62; see the isolation
argument below for why this is fine). `fpm_connection_info()` is that
report, straight from the bufferevent and its SSL object.

```php
$info = fpm_connection_info();
```

Returns `false` when there is nothing to report — no request currently in
flight (e.g. called outside of the direct-request lifecycle) — or any pool
that is not `pool.type = http-direct` in the first place — `function_exists
('fpm_connection_info')` is `false` there, since the function is registered
per-pool at startup, not globally.

**`pool.executor = worker`** (issue #335): this executor answers by request
id, since several requests can be in flight against one PHP engine at once —
`fpm_connection_info(int $id = 0)`. There is no ambient "current request" the
way classic has, so an omitted or zero `$id` returns `false` unconditionally,
the same as an `$id` this worker never handed out, or one whose request has
already been answered. Otherwise the array is the same shape described below,
with two keys always missing rather than faked: `age` and `requests`. This
executor deliberately drops its per-connection bookkeeping the moment the
first request off a connection arrives, to avoid leaking one fd's worth of
state for the life of the worker (the same `track_live` trade-off the classic
executor makes the opposite way) — there is therefore no accept time and no
served-request count it can honestly report, and it says so by omitting the
keys instead of inventing a value, the same "report honestly, omit what
genuinely cannot be known" convention issue #333 established for worker
metrics. TLS/client-cert fields are produced by the same code the classic
executor uses (`evhttp_connection_get_bufferevent()` reached from the
pending request's own `evhttp_request*`), so everything below except `age`
and `requests` applies unchanged to a worker pool.

Otherwise returns an array, always with:

| key | type | meaning |
|---|---|---|
| `transport` | string | `"tls"` or `"plain"` |
| `peer_addr` | string | client IP address |
| `peer_port` | int | client source port |
| `age` | float | seconds since this connection was accepted |
| `requests` | int | requests served on this connection so far, including the current one |

When `transport` is `"tls"`, four more keys are always present:

| key | type | meaning |
|---|---|---|
| `tls_protocol` | string | e.g. `TLSv1.3` |
| `tls_cipher` | string | negotiated cipher name |
| `tls_alpn` | string\|null | ALPN protocol the client negotiated, if any |
| `tls_sni` | string\|null | SNI hostname the client sent, if any |

`client_cert_*` keys exist only when this pool's `http.tls_verify_client` is
not `none` — checking `http.tls_verify_client` once (e.g. against a pool-wide
constant) is enough to know whether to expect them, without inspecting
`array_key_exists()` per request:

| key | type | meaning |
|---|---|---|
| `client_cert_verified` | bool | `true` iff a certificate was presented and chains to `http.tls_client_ca` |
| `client_cert_subject` | string\|null | `X509_NAME_oneline()`, e.g. `/CN=client.test` |
| `client_cert_issuer` | string\|null | same format |
| `client_cert_not_before` | string\|null | `YYYY-MM-DDTHH:MM:SSZ` |
| `client_cert_not_after` | string\|null | `YYYY-MM-DDTHH:MM:SSZ` |
| `client_cert_fingerprint_sha256` | string\|null | lowercase hex SHA-256 of the DER encoding, no separators |

Under `http.tls_verify_client = optional`, a connection whose client
presented no certificate at all still gets all six `client_cert_*` keys —
`client_cert_verified` is `false` and the rest are `null`, rather than the
keys being absent, so a script does not have to special-case "no keys" versus
"keys present but empty."

**Deliberately not exposed**: the raw socket/file descriptor (a script gets
facts about the connection, never a handle it could use to touch the socket
directly, bypassing the SAPI); the server's own certificate or private key
material; and anything about a connection other than one that has an actual
request pending against it right now. Classic's `fpm_connection_info()` takes
no id at all, since it only ever has one request in flight to mean; the
worker executor's `$id` (issue #335) is not a query interface over arbitrary
connections either — it only resolves an id this same worker itself hasn't
yet answered, never another worker's or another pool's state. This keeps the
API's trust model simple: everything it returns describes facts the worker
itself observed on the wire for *a* request it is actually handling, not a
general connection directory.

**Trust model**: these are the worker's own observations of its own TLS
session, not values a request could inject or a proxy could have rewritten in
transit — there is no intermediary between the client and this code, unlike
FastCGI where equivalent-looking `$_SERVER` values would only be as
trustworthy as whatever sits in front of PHP. `client_cert_verified` reflects
OpenSSL's own chain validation against `http.tls_client_ca`
(`SSL_get_verify_result() == X509_V_OK`), the same validation that decides
whether `require` accepts the handshake at all.

**Read-only and per-request isolation**: `fpm_connection_info()` has no
corresponding setter; it cannot be used to affect the connection, only to
observe it. Task 054's cross-request isolation guarantees extend to it: a
connection's fields (TLS session state, and on classic, `age`/`requests`)
never leak into a different connection's requests, and a worker that has
served no request in the current call (classic), or an id that names no
pending request (worker executor), sees `false`.

Covered by `sapi/fpmng/tests/fpmng-http-direct-connection-info.phpt`: a plain
pool returning transport facts with no TLS keys; `tls_verify_client = none`
never exposing `client_cert_*` even when the client offers a certificate;
`optional` with no client certificate, with a CA-trusted one (cross-checked
against `openssl s_client`'s own view of the same handshake), and with an
untrusted one (handshake proceeds, `client_cert_verified` is `false`);
`require` rejecting both a missing and an untrusted client certificate at the
handshake layer; a `pool.type = http` gateway pool never defining the
function at all; and, before issue #335, `pool.executor = worker` returning
`false` unconditionally. `pool.executor = worker`'s own id-based behavior —
a valid id reporting `peer_addr`/`peer_port`/`transport` with `age`/`requests`
absent, and a missing/zero/unknown id returning `false` — is covered
separately by `sapi/fpmng/tests/fpmng-http-direct-worker-connection-info.phpt`,
on a plain (non-TLS) pool; the TLS/client-cert fields are the same code path
already exercised above, so that test does not duplicate a TLS harness for
the worker executor.

## Protocol passthrough: early hints, status codes, and methods (issue #63)

A direct pool's script *is* the server, so nothing between it and the wire
filters what it can say the way nginx or the `http` gateway does — this
section covers the three protocol features issue #63 asked about, one of
which turned out to already be there, one of which is new, and one of which
is not supported and, on the libevent this project links, cannot be added
without a much larger change.

### Custom response status codes: already unrestricted

`fpm_http_direct_status_final()` accepts any status 200–599 the application
sets, via `http_response_code()` or a raw `header('Status: ...')` line — a
non-standard code (e.g. `Status: 299`) reaches the wire exactly as PHP built
it, unmodified. The only statuses this SAPI refuses are outside 200–599:
1xx (see [`fpm_send_early_hints()`](#fpm_send_early_hintsarray-headers-bool)
below for why), and anything above 599, which is not a valid status line at
all. No code change was needed for this — it is existing behavior,
documented here because issue #63 asked for it explicitly.

### `fpm_send_early_hints(array $headers): bool`

Sends a 103 Early Hints response (RFC 8297) immediately, ahead of the final
response this request will eventually send — the same idea as
`Link: </style.css>; rel=preload` today, except the client can start
fetching it before PHP has finished computing anything.

```php
fpm_send_early_hints(['Link' => '</style.css>; rel=preload']);
// ... the application keeps computing ...
header('Content-Type: text/html');
echo $html;
```

Each array value may be a string or an array of strings (repeats the header,
same convention `header()` and the worker executor's `fpmng_worker_respond()`
use for a multi-valued header, e.g. multiple `Link` values). Returns `true`
once the interim response has been written; `false`, having written nothing,
when:

- there is no request currently in flight (same convention as
  `fpm_connection_info()`/`fpmng_respond()`);
- the final response has already started going out — headers already sent,
  `fpmng_respond()` already called, or (`http.stream`) streaming already
  began. A 103 has to precede the final status line; once that line may be on
  the wire, sending more bytes ahead of it would corrupt framing a client
  reads strictly in order;
- the client is HTTP/1.0, which has no notion of a 1xx interim response and
  would read these bytes as garbage before the one status line it expects;
- on `pool.executor = worker` (see below), the id is missing/zero or unknown.

Can be called more than once per request; RFC 8297 allows several 103
responses before the final one. Header validation is the same chain the
final response uses (a name that is not an RFC 9110 token, or
`evhttp_add_header()`'s own CRLF/injection check), but a rejected header is
silently dropped from the 103 rather than failing the whole call — unlike
the final response, which answers 500 on the same failure, since the 103 is
optional by nature and the final response is still to come. Each call gets
its own fresh 64 KiB header budget (`fpm_http_direct_header_charge()`),
independent of whatever the final response separately spends its own budget
on — a large 103 cannot starve the final response's header allowance, or the
other way around. Framing-owned headers (`Content-Length`, `Connection`,
...) are dropped the same way the final response drops them — a 103 has no
body and must not claim a connection-management semantic.

**Why this needs its own code path rather than reusing the final response's
plumbing**: evhttp (this project's HTTP layer, from libevent) has no
interim-response API — its reply functions
(`evhttp_send_reply()`/`evhttp_send_reply_start()`) each frame whatever
status they are given as *the* response, which is exactly why
`fpm_http_direct_status_final()` excludes 1xx: routing a 103 through either
of them would be read by libevent itself as the final answer, with body and
connection-management framing implications, corrupting the wire for the real
response still to come. `fpm_send_early_hints()` instead writes the interim
response as literal bytes straight to the connection's `bufferevent` (the
same one `fpm_connection_info()` reaches for its TLS fields), never touching
evhttp's request/reply state — the real response afterwards goes out through
the normal path exactly as if this had never been called.

**`pool.executor = worker`** (issue #335): `fpm_send_early_hints(array
$headers, int $id = 0)` — the worker executor's own version takes the same
kind of id `fpm_connection_info()` does, since several requests can be in
flight against one PHP engine and there is no ambient "current request" to
default to; a missing/zero `$id`, or one this worker never handed out,
returns `false`. Given a valid id, the guard replacing classic's `r->responded
|| r->streaming || SG(headers_sent)` is: the id must still be pending (an id
already answered outright by the buffered `fpmng_worker_respond()` has already
been reaped out of the pending table, so it resolves to nothing — the same as
an unknown id) and its pending entry's streaming flag (set by
`fpmng_worker_respond_start()` from issue #332, cleared by
`fpmng_worker_respond_end()`/an aborted stream) must not be set — a final
response already streaming out is exactly the "may already be on the wire"
condition classic's flags describe. Header validation, serialization (a
"HTTP/1.1 103 Early Hints" status line, the validated headers, then a blank
line), and the write to the connection's `bufferevent` — resolved via
`evhttp_connection_get_bufferevent(evhttp_request_get_connection())` from the
pending entry's `evhttp_request*` instead of `r->conn_bev` — are otherwise
identical to classic's implementation, reusing the same header-validation
chain `fpm_worker_add_header()` already uses for the final response. The
HTTP/1.0 check is unchanged: `p->http->major`/`->minor` are the same
`evhttp_request` fields classic checks.

Covered by `sapi/fpmng/tests/fpmng-http-direct-early-hints.phpt`: a raw-socket
client observing the exact wire order and byte-for-byte framing of a 103
followed by the final response (headers included); early hints ahead of a
bodyless (HEAD/204) final response, checked for keep-alive correctness on the
same connection afterwards; a malformed header name dropped from the 103
without failing the call; and, before issue #335, `pool.executor = worker`
returning `false` unconditionally (unchanged: that test still calls the
1-arg form with no id, still `false`). `pool.executor = worker`'s own
id-based success/failure cases — wire order on a successful send, refusal
once a streamed response has started, refusal for HTTP/1.0, and refusal for
a missing/unknown id — are covered separately by
`sapi/fpmng/tests/fpmng-http-direct-worker-early-hints.phpt`.

### Custom / arbitrary request methods: not supported (infeasible on this libevent)

Issue #63 asked for arbitrary methods (e.g. `REPORT`) to reach PHP in
`$_SERVER['REQUEST_METHOD']`, "cheap to support once" if the transport can
pass them through. It cannot, without a change well beyond this issue's
scope: `evhttp_set_allowed_methods()` and `evhttp_request_get_command()` — the
only API this project's HTTP layer offers for restricting or reading a
request's method — operate on `enum evhttp_cmd_type`
(`event2/http.h`), a **closed, fixed bitmask of exactly nine values** (`GET`,
`POST`, `HEAD`, `PUT`, `DELETE`, `OPTIONS`, `TRACE`, `CONNECT`, `PATCH`).
There is no `EVHTTP_REQ_CUSTOM` or equivalent escape hatch in libevent 2.1.x
(confirmed against the installed `event2/http.h`, and stable across the
2.1.x series this project's supported distributions ship). evhttp parses the
request line and rejects any method token outside that set before this
codebase's request callback ever runs — the restriction is enforced inside
evhttp itself, not by anything in `fpm_http_direct.c`/`fpm_http_direct_worker.c`
that could simply be relaxed.

Reaching a truly arbitrary method would mean either patching libevent itself
or bypassing its HTTP request-line parser entirely for a raw
`bufferevent`-level implementation. This project patches php-src for its own
worker/fiber transport needs (`patches/0001`–`0005`; 0006 was removed by issue
#420 and 0007/0008 live on branch async) but has never carried a
libevent patch, and vendoring or patching a system HTTP parsing library is a
materially larger commitment (a new patch surface to track across
distributions' own libevent updates, plus request-line parsing security
review) than this issue's "cheap to support once" framing anticipated. This
is left unimplemented and documented here rather than forced through with a
hand-rolled request-line parser; TRACE and CONNECT, which *do* exist in
evhttp's enum, remain deliberately unmapped too (see
[Deliberate limits](#deliberate-limits)) — TRACE for the same reason most
HTTP servers refuse it by default (cross-site tracing, RFC 9110 §9.3.8's own
security note), CONNECT because this transport has no tunnel to offer one
through.

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
