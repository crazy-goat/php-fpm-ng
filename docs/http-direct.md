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

- No trusted-proxy handling, gateway ACLs, gateway access log, HTTP/2,
  WebSocket upgrades, or CONNECT/TRACE. TLS is supported — see below. Static
  files are too, opt-in and on the classic executor only — see below as well.
  The pool-level operator directives (`ping.path`, `pm.status_path`,
  `access.log`, `listen.allowed_clients`, `chroot`) are supported as well — see
  [Operating a direct pool](#operating-a-direct-pool).
- Only `http.front_controller`, `http.max_body`, `http.read_timeout`, the
  `http.tls_*` group and — on `pool.executor = classic` only — `http.stream`,
  `http.stream_write_timeout`, `http.static`, `http.max_connections` and
  `http.max_connections_per_client` from the gateway's `http.*`
  directives are accepted. Other
  gateway options are rejected, even if explicitly set to an otherwise harmless
  default. `listen` is the HTTP endpoint; `http.listen` does not apply.
- `chdir` must be absolute. `pm.status_listen` is rejected on both executors: it
  asks for a second listening socket served by a second process, and a direct
  child owns exactly one listener — the pool's.
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

A worker may hold **at most 256 accepted-but-unanswered requests at a time**
(`FPM_WORKER_PENDING_MAX`, `sapi/fpmng/fpm/fpm_http_direct_worker.c:76`). That
is the concurrency limit of deferred replies, and it is a hard one: reaching it
does not merely throttle, it retires the worker.

The transport cannot tell a request held on purpose — the long-poll that
`fpmng_worker_respond()` exists to answer later — from one leaked by a handler
that returned without answering, because a pending entry is removed only by
`fpmng_worker_respond()` (`fpm_worker_reap()`, `:351-354`). **This decision
resolves that ambiguity against the deliberate case, on purpose**: on the
257th concurrent request `fpm_worker_accept()` (`:368-401`)

- answers that request `503 Worker unavailable`,
- logs `WARNING … 256 requests accepted but unanswered`,
- sets the stop flag, so the worker script is asked to stop and the master
  respawns the child — and `fpm_worker_finish_output()` (`:453-489`) then
  answers **every one of the 256 held requests** `503` as well, logging how
  many it abandoned.

So a handler that deliberately parks 256 long-polls loses all of them. The
alternative — a way for the script to mark a request as held on purpose — is
not implemented, because nothing else in the process would then bound it: the
master-side deadlines that would normally catch a stuck request
(`request_terminate_timeout`, `request_slowlog_timeout`, `slowlog`) are
*rejected* at startup by this executor (`:167-175`), since the worker script
never ends a request and the child never leaves its stage. A leak that the
transport did not catch would therefore be caught by nothing at all, for the
life of the worker. Given a POC whose long-lived-connection story is still
being measured (#68), a bounded false positive for long-polling was preferred
over an unbounded false negative for leaks.

Two consequences follow from the same "the child never ends a request"
property and are **not** bugs in the above, but they are easy to trip over:

- `pm.max_requests` counts only *answered* requests (`:1071-1078`), so a worker
  that holds requests forever never recycles on that trigger. The pending
  ceiling is the only thing that eventually retires it.
- The child reports `ACCEPTING` for its whole life (`fpm_request_accepting(false)`
  is called once, `:1512`), so `fpm_request_is_idle()`
  (`sapi/fpmng/fpm/fpm_request.c:306-316`) — and therefore `pm = ondemand`
  bookkeeping and the scoreboard — see a worker holding 200 long-polls as idle.
  Per-request accounting for this executor is #64.

If a supported long-polling shape ever needs more than 256 held requests per
worker, or needs them to survive the ceiling, that is a design change: it has
to come with a bound of its own, and it belongs to #68 and its follow-ups, not
to this limit.

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

**On a TLS pool it only moves the accounting.** The response waits for the event
loop exactly as it does without the call, because that direct write is refused
there: a TLS bufferevent holds plaintext in the buffer the write would drain and
the encrypted session on the descriptor it would drain it to, so writing one to
the other would put the response on the wire in the clear. This is the same
constraint that makes `http.stream` incompatible with `http.tls_cert`.

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
pm.status_path = /status
access.log = /var/log/php-fpm/app.access.log
access.format = "%R - %u %t \"%m %r%Q%q\" %s %{milli}d %{kilo}M"
access.suppress_path[] = /ping
listen.allowed_clients = 10.0.0.4,10.0.0.5
chroot = /srv/jail
```

### `ping.path` and `pm.status_path`

Both are answered by the worker's own event loop on the pool's listener, before
any PHP request is started, and neither counts against `pm.max_requests`. They
are matched against the request path with the query string cut off, and matched
*whole*: `/statuses` is the application's URL, not the status page. There is no
percent-decoding — the directive is a literal in the pool file and upstream
matches it literally too, so `/%73tatus` is not a way past a proxy rule written
against the documented spelling.

`pm.status_path` answers plain text, or JSON for `?json`, with the same
`Expires`/`Cache-Control` headers upstream's `fpm_status.c` sends. The fields:

| Field | Where it comes from |
|---|---|
| `pool`, `process manager`, `start time`, `start since` | the pool's scoreboard |
| `idle processes`, `active processes`, `total processes`, `max active processes`, `max children reached` | the pool's scoreboard |
| `requests`, `slow requests`, `memory peak` | the pool's scoreboard — PHP requests only |
| `accepted conn` | connections this pool's children accepted, counted in the one hook libevent runs per accepted connection |
| `non-php requests` | static files, pings and status pages: answered without starting a PHP request |
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
| 0 | 9534 rps | 9460 rps | 9789 rps |
| 500 | 9555 rps | 9180 rps | 9651 rps |
| 2000 | 9686 rps | 6708 rps | 9506 rps |

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

`pm.status_listen` stays rejected on both executors: it asks for a second
listening socket served by a second process, and a direct child owns exactly
one listener — the pool's.

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
- Responses that never ran PHP — a static file, a ping, the status page, a
  `403` or a `503` — are logged too, with the fields that do not apply (`%M`,
  `%C`, `%f`, `%u`) left at zero or `-` rather than carried over from whatever
  this child served last.
- `access.suppress_path[]` matches the same request path.

Under `pool.executor = worker` all three of `ping.path`, `pm.status_path` and
`access.*` are **rejected**, for the same reason `request_terminate_timeout` is:
that executor calls `fpm_request_accepting(false)` once for the life of the
child, so there is no per-request stage, duration, CPU or peak memory to report
and no request to count. Refusing the directive is better than answering it
with placeholders.

### `listen.allowed_clients`

A comma-separated list of literal IPv4/IPv6 addresses, the same matching rules
FastCGI's `listen.allowed_clients` uses (no CIDR; an IPv4 peer also matches an
allowed IPv4-mapped IPv6 entry). It shares the matcher with the gateway's
`http.allowed_clients` (`fpm_http_acl.c`).

It is enforced in the request callback, not at accept: libevent's
`evhttp_set_bevcb()` runs before the peer address is known. The TCP connection
is therefore accepted and the request answered `403`, which is also what the
`http` gateway does. A refused request reaches neither the static file server,
nor the status page, nor PHP, and is counted as `refused acl` (and inside the
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
