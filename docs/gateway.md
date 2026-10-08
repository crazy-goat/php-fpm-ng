# The gateway: ping, metrics and status across pool types

This is the living gateway guide: it covers gateway routing, operator-page
forwarding, ping, and the gateway's own metrics. The canonical directive table,
listener defaults, collision rules, and per-pool operator-page contract live in
[`operator-endpoint.md`](operator-endpoint.md); consult that page when
configuring `operator.*`. `pool.type = http` is retired: configure a `pool.type = gateway`
proxy plus a separate `fastcgi` or `http-direct` pool for PHP.

## Why

`pool.type = http` is two things welded into one section: a pool of PHP
workers, and a proxy in front of them. Every awkward question about monitoring
comes from that weld -- whose status page is it, the workers' or the proxy's;
what does `/metrics` mean on a pool that has gateway processes, PHP children and
a shared operator child; why does `/ping` reach the application. Separate the
two and the questions do not get answered, they stop being asked.

The gateway becomes a type of its own: a minimal proxy, like nginx with a
minimal configuration, that routes to other pools and exposes their operator
pages. It runs no PHP. It has no process manager. Every route is explicit.

## Three rules

Everything below follows from these. They are stated once so nobody re-derives
them differently.

1. **`ping.path` is answered on the request listener, by a process that serves
   requests there.** Never on the operator listener. The probe's whole value is
   that it walks the same accept queue as a real request; answered anywhere
   else it proves nothing. A type with no request listener (`cron`,
   `supervisor`) has no ping and refuses the directive. On the gateway the
   request listener *is* the gateway process, so the gateway answers it itself,
   before routing (#382) -- proving the gateway is alive, not its targets. A
   target is pinged on its own listener.

2. **Metrics are answered on the operator listener, one path per pool, never on
   the request listener.** They must answer when every worker is busy, and they
   must have one exposition format on every type so one scraper compares
   series across pools.

3. **Status follows metrics onto the operator listener**, with the type's own
   page where the type has one (`http-direct`). On `pool.type = fastcgi`,
   upstream's `pm.status_path` keeps its meaning -- a path on the pool's own
   FastCGI socket, for the web server in front. The pool may separately opt in
   to an operator status page with `operator.status_path` or `operator.status`.

Rules 2 and 3 look symmetrical to rule 1 and are its exact opposite. That is
the thing to remember.

## Directives

### `operator.*` -- directive reference

The operator listener is separate from the process manager, and the directives
use the `operator.*` namespace on every supported pool type, including FastCGI.
`operator-endpoint.md` is the canonical reference for path and listener
directives, shorthand flags, defaults, path-safe pool names, and collision
rules. `pm.status_path` remains upstream-only on FastCGI and is separate from
`operator.status_path`.

Gateway-specific defaults: its own status and metrics pages are on by default
at `/status` and `/metrics`; set `operator.status = off` or
`operator.metrics = off` (or an empty path) to disable one. All gateway and
pool operator listeners default to `127.0.0.1:9253`, so pools that omit
`operator.*_listen` share one listener process. Explicit listen addresses are
only needed to split endpoints across interfaces/firewall rules; they are not
required for the gateway-first topology below.

### `http.*` -- on the gateway

Existing `http.*` directives keep their names. They configure what the gateway
does on its public port -- TLS, static files, body limits, routing -- and that
is honest for a type called `gateway`. Two are new:

| Directive | Meaning | Default |
| --- | --- | --- |
| `http.route[<pool>]` | Comma-separated path prefixes routed to `<pool>`. | -- |
| `http.operator` | Serve every exposed pool's operator pages through this public port. | `no` |
| `http.operator_allowed_clients` | Who may reach them. Separate from `http.allowed_clients`. | -- (required when `http.operator = yes`) |

Client-side limits (issue #593). A gateway process serves every connection of
its pool, so a client that holds a socket open holds a file descriptor for all of
them:

| Directive | Meaning | Default |
| --- | --- | --- |
| `http.read_timeout` | Budget in ms for reading one whole request (headers and body). The first request from accept; every later request on the connection from its first byte. `0` = off. | `5000` |
| `http.keepalive_timeout` | How long in ms an idle keep-alive connection may wait for its next request. `0` = unlimited. | `60000` |
| `http.write_timeout` | How long in ms a client may make no progress on a pending response before the connection is closed. `0` = unlimited. | `30000` |

Upstream limits (issue #716). What the gateway waits *for* on the target side. A
target that never answers used to hold the client connection and one slot of the
target's budget indefinitely -- with the default `request_terminate_timeout = 0`
nothing ended it, and a fiber target refuses that directive at all (#610):

| Directive | Meaning | Default |
| --- | --- | --- |
| `http.upstream_connect_timeout` | How long in ms a connect towards a target may stay unfulfilled (a SYN nobody answers, an accept queue the kernel has stopped draining). Cut after it: `504` if nothing was sent to the client yet. `0` = wait forever. | `5000` |
| `http.upstream_read_timeout` | How long in ms a target may make **no progress** on a request in flight. Cut after it: `504` if the response head has not been sent, otherwise the client connection is closed without completing the reply (the rule of #533, since the body can no longer be made whole). `0` = never cut. | `60000` |

`http.upstream_read_timeout` bounds time **without progress**, not total request
time: every byte the target sends re-arms it, so a script that answers slowly but
keeps producing is not cut. A script that computes for a minute before its first
byte is, and the operator sees the 504 they would have seen from the proxy the
gateway replaces. Raise it, or set `0` to keep waiting as before, for anything
that legitimately takes longer.

The two defaults are judgement calls, not measurements:

- `5000` for the connect, which is the number `http.read_timeout` above already
  uses. The target is on this host in every configuration this project ships -- a
  Unix socket, or a loopback address, which is the only address a route may even
  name for an `http-direct` target -- so a TCP connect that has not completed
  within the time it takes to read a whole request is not a slow peer, it is a SYN
  nobody will answer. Not measured: how long such a connect takes when it does
  complete. A remote FastCGI target is allowed (unlike an `http-direct` one) and
  should get a larger value.
- `60000` for the read, which is nginx's `proxy_read_timeout` default and the same
  minute `http.keepalive_timeout` above already uses.

While the gateway has paused an upstream because the client is behind
(`http.response_buffer`), the read deadline is stopped: the target is producing
into a socket buffer nobody is draining, which is not silence, and
`http.write_timeout` is what bounds that client.

Both are refused on `http-direct`, like the gateway-only client limits above: a
direct pool is the thing serving the request and has no upstream to wait for.

Response flow control (issue #596). The gateway reads the upstream response
only while the client keeps up:

| Directive | Meaning | Default |
| --- | --- | --- |
| `http.response_buffer` | Bytes of response the gateway keeps unwritten for one client. Above this it stops reading that request's upstream (the worker blocks in its write) until the client has drained the buffer. `0` = unlimited. | `1M` |
| `http.response_min_rate` | Minimum bytes per second the client must drain while the upstream is held back for it. Below that the connection is closed and the worker is released. `0` = no minimum. | `256` |

The gateway counts each pause. Read `fpmng_gateway_responses_paused_total` for the number of
pauses. Read `fpmng_gateway_responses_paused` for the number of responses that are paused at
this time. See [The gateway's own numbers](#the-gateways-own-numbers) (issue #706).

A larger `http.response_buffer` frees a PHP worker earlier for a slow client and costs gateway
memory per slow client; a smaller one bounds the memory and holds the worker longer. The limit
is checked after each piece of the response, so one read (16 KiB) can overshoot it, and the
kernel socket buffers on the client and upstream side come on top. The write timeout above
still closes a client that reads nothing; with `http.write_timeout = 0` such a client keeps its
connection and one worker, but no longer grows the gateway's memory. Not measured: the
gateway's RSS under many slow clients. `http.response_buffer` and `http.response_min_rate` are
refused on `http-direct`.

`http.response_min_rate` is the minimum-progress rule that closes the trickle-reader path
`http.response_buffer` opened (issue #705). It is measured over a fixed 5-second window and
only while the upstream is actually held back, which is the only time the client, and not the
upstream, is the bottleneck: the client must have drained at least `rate * 5` bytes in the last
5 seconds, or the connection is closed the same way `http.read_timeout` closes one, and the
worker's remaining output is drained as if the client had left. A legitimately slow but
progressing download is therefore kept, however long it takes; only a client that has
effectively stalled while still consuming a byte now and then is cut. A rate below
`http.response_buffer / 5` (with the defaults, about 200 KiB/s) is the meaningful range: a
client that can empty the whole buffer inside one window is never cut, because draining the
buffer resumes the upstream and stops the clock. `http.write_timeout` remains the limit for a
client that stops entirely.

Two consequences of holding the worker back, both new with flow control:

- **Blocked time counts against the target's own limits.** While the gateway
  has stopped reading, the target worker is blocked in its write, and that wall
  time is charged to it. A streaming `http-direct` target (`http.stream = yes`)
  spends it from `http.stream_write_timeout`, a *total* budget per response
  (default 10000 ms): a client that lags behind the target for longer than that
  in total gets a response cut without its terminating chunk. A FastCGI target
  spends it from `request_terminate_timeout`. Before this change the gateway
  took the whole response and the target never blocked. Measured: a 128 MiB
  streamed response to a client that waits 15 s before reading arrived
  truncated (5.4 MB) with the defaults and complete with
  `http.response_buffer = 0`. Behind a gateway, give streaming targets a
  `http.stream_write_timeout` (and FastCGI targets a `request_terminate_timeout`)
  at least as long as the slowest download you want to serve, or set
  `http.response_buffer = 0` to keep the old behaviour (the gateway buffers
  everything, bounded only by memory). `fpmng-http-gateway-stream-budget.phpt`
  pins both outcomes.
- **A trickling reader no longer holds a worker indefinitely.** `http.write_timeout`
  is a stall timer: it restarts whenever the client takes any bytes, so a client
  that reads one byte per second is never cut by it, and with flow control such a
  client keeps a PHP worker (or target worker) blocked for as long as it trickles
  -- a few of them can occupy all of `pm.max_children`. `http.response_min_rate`
  closes that path: while the upstream is held back, a client that drains less
  than the configured rate over a 5-second window is cut and the worker is
  released, while a slower but steadily progressing client is kept (see the flow
  control section above). `fpmng-http-gateway-min-rate.phpt` pins both outcomes.

`http.plain_listen` has the first-request deadline and the keep-alive limit too.
`http.idle_timeout` is **not** a client timeout: it is the upstream-side timer.
`http.max_connections` is the cap on the client connections that one gateway
process holds. The cap counts idle keep-alive connections too. With
`http.gateways = 2`, the pool can hold twice the number. At the cap, the process
stops accepting: a new client waits in the listen backlog until a connection
closes. The process does not refuse that client.
A client that opens a connection and sends no request keeps its slot until
`http.read_timeout` closes the connection, or until the client closes it. The
default is `5000`. With `http.read_timeout = 0` no deadline applies, so such a
client holds its slot until it closes.

`http.max_connections_per_client` caps the connections from one peer address
inside the process cap. A client over this cap gets one of two answers: the
process closes the connection without a response, or it sends `503`. A client
must handle both. Both values must be between 0 and 1000000, and 0 means
unlimited. The per-client cap requires `http.max_connections` and must not be
above it. `php-fpm-ng -t` refuses a configuration that breaks these rules.
`http.keepalive_timeout`, `http.write_timeout`, `http.response_buffer`,
`http.response_min_rate` and the two `http.upstream_*` timeouts are refused on
`http-direct`.

`http.operator*` stays in `http.`, on purpose: it does not configure the
operator listener, it configures what the gateway does with its own port.

`http.allowed_clients` is this listener's ACL. `listen.allowed_clients` is a
FastCGI-worker ACL and is **refused** on a gateway (issue #493): a gateway has
no worker socket -- `listen` *is* the public port -- so accepting it would leave
an operator who wrote it believing the public listener was restricted while it
served everyone. On the retired combined `http` pool it restricted the FastCGI
half, never the public port; use `http.allowed_clients` here.

`http.route[]` is keyed by pool name (#340): the key validates itself against
the configured sections, the value is free to grow a pattern syntax later, and
an INI key cannot sensibly hold `/`, `.` or `|`. Several prefixes may name one
pool; they share that pool's budget and queue, because one set of workers
enforces it.

A gateway pool that routes to a FastCGI pool needs a docroot: `chdir` (the
document root `SCRIPT_FILENAME` is built under) and, unless every routed path
names an existing `.php` file, `http.front_controller` (the fallback script
for paths that do not). With neither set, every request names a script that
does not exist and the FastCGI upstream answers "Primary script unknown"; the
master logs a WARNING saying so at startup.

When every worker of a routed target is busy, the gateway answers `503` +
`Retry-After` immediately (`http.pool_full_policy = reject`, the default; up to
100 ms later with `http.gateways > 1` when a sibling gateway process holds the
workers, see the "Several gateway processes" section of
[`http-gateway-pool-full.md`](http-gateway-pool-full.md));
[`http-gateway-pool-full.md`](http-gateway-pool-full.md) covers the opt-in
`wait` alternative.

**Cleartext routing boundary.** FastCGI targets use their FastCGI socket. An
`http-direct` target is contacted over cleartext HTTP/1.1, so its `listen` must
be a Unix socket, a numeric IPv4 address in 127/8, or the IPv6 loopback literal
`::1`. Public and wildcard addresses, hostnames (which could resolve or rebind
to a public address), IPv4-mapped IPv6 addresses and other non-loopback targets
are refused by `php-fpm-ng -t`; TLS-terminating
`http-direct` targets remain refused too. To route over the network, use a
transport with TLS rather than exposing the gateway's cleartext target hop.

### Access log fields and request id (issue #642)

The gateway writes one line for each request to `http.access_log`. The Combined
Log Format fields keep their place. The gateway adds these fields after
`target=`:

```
203.0.113.7 - - [08/Oct/2026:10:00:00 +0000] "GET /api/items HTTP/1.1" 200 812 "-" "curl/8.5.0" target=api duration_ms=14 upstream_ms=11 request_id=3f9c2a1e7b4d4f0e9a6c1d2b3e4f5a6b
```

| Field | Meaning | When it is printed |
| --- | --- | --- |
| `duration_ms` | Time from the end of the request read to the write of the line. | Always. `-` when the gateway did not record the start of the request. |
| `upstream_ms` | Time from the hand-off to a target to the write of the line. | Always. `-` when no target got the request. For example: a ping, a static file, or a `503` sent before the hand-off. |
| `queue_ms` | Time the request waited for a free worker. This is the same value as the `X-Fpmng-Queue-Wait` header. | Only with `http.pool_full_policy = wait`. `0` when the request did not wait. A request that expires its wait bound logs the whole wait. Other rejections omit the field. |
| `request_id` | The id of the request. See `http.request_id`. | Only when `http.request_id` is `generate` or `propagate` and the gateway has an id. |

The layout of the line is set by `http.access_format`. See [Access log format](#access-log-format).

`http.request_id` gives one id to each request. The gateway writes the id to the
access log, sends it to the target, and sends it back to the client:

| Directive | Meaning | Default |
| --- | --- | --- |
| `http.request_id` | `off`: no id. `generate`: the gateway makes a new id for each request. `propagate`: the gateway keeps a valid inbound id from a trusted proxy and makes a new id for every other request. | `off` |

The rules:

- A generated id has 32 lowercase hexadecimal characters. The gateway makes it from 128 random bits from `getentropy()`.
- With `propagate`, the gateway keeps the inbound `X-Request-Id` header only when the TCP peer of the connection is in `http.trusted_proxies`. The address in `X-Forwarded-For` does not count.
- An inbound id has 1 to 128 characters. Each character is one of `A-Z`, `a-z`, `0-9`, `.`, `_` or `-`. The gateway replaces any other value with a new id. It never cuts an id.
- A FastCGI target gets the id in `HTTP_X_REQUEST_ID`. An `http-direct` target gets the id in an `X-Request-Id` header.
- With `generate` or `propagate`, the gateway does not send the client's `X-Request-Id` header to the target. With `off`, the gateway forwards that header like any other header.
- The client gets the id in an `X-Request-Id` response header. If the target sends its own `X-Request-Id` header, the gateway id wins.

The default is `off`. With `off`, the gateway sends no `X-Request-Id` header and
writes no `request_id=` field. `http.request_id` is refused on `http-direct`, as the
other gateway limits are. The gateway does not read a `traceparent` header. It
forwards `traceparent` to the target like any other header.

An `http-direct` pool does not make an id. Its `access.format` can print the
`X-Request-Id` header that the pool receives, with `%{HTTP_X_REQUEST_ID}e`. The pool
does not check that header. Put the pool behind a gateway with `http.request_id` set,
or accept the value that the client sends.

Limit: the gateway sends the `X-Request-Id` response header only with the replies that
it writes with its own headers. A reply written by `evhttp_send_error()` does not carry
it, because that call clears the response headers first. Examples are `403` and `404`,
and some `502`, `503` and `504` replies. The pool-full `503` does carry the header,
because the gateway writes it with `evhttp_send_reply()`. The access log line of such a
request still has the `request_id=` field.

### Access log format

`http.access_format` sets the layout of each line in `http.access_log`.

| Directive | Meaning | Default |
| --- | --- | --- |
| `http.access_format` | `combined`: the Combined Log Format with the fields above. `json`: one JSON object for each request, with the keys below. | `combined` |

The gateway writes each line with one `write()` call in both layouts. The directive is
refused on `http-direct`.

With `json`, every line has the same keys in the same order. An unknown value is `null`,
not `-`. The keys `queue_ms` and `request_id` are `null` when `combined` leaves them out.

This is one line for a request that went to the `api` target:

```
{"time":"08/Oct/2026:10:00:00 +0000","remote_addr":"203.0.113.7","remote_user":null,"method":"GET","uri":"/api/items","protocol":"HTTP/1.1","status":200,"bytes":812,"referer":null,"user_agent":"curl/8.5.0","target":"api","duration_ms":14,"upstream_ms":11,"queue_ms":null,"request_id":"3f9c2a1e7b4d4f0e9a6c1d2b3e4f5a6b"}
```

| Key | Type | Value |
| --- | --- | --- |
| `time` | string | The request time, in the Combined Log Format form. |
| `remote_addr` | string or `null` | The client address. |
| `remote_user` | string or `null` | The user name from the `Authorization` header. |
| `method` | string or `null` | The request method. |
| `uri` | string or `null` | The request target. |
| `protocol` | string | The protocol, for example `HTTP/1.1`. |
| `status` | number or `null` | The status code. `null` when the connection failed before a response. |
| `bytes` | number | The body bytes sent. |
| `referer` | string or `null` | The `Referer` header. |
| `user_agent` | string or `null` | The `User-Agent` header. |
| `target` | string or `null` | The `http.route[]` target. `null` where `combined` prints `target=-`. |
| `duration_ms` | number or `null` | As in the field table above. |
| `upstream_ms` | number or `null` | As in the field table above. |
| `queue_ms` | number or `null` | As in the field table above. `null` where `combined` omits the field. |
| `request_id` | string or `null` | The request id. `null` where `combined` omits the field. |

The escaping rules for strings:

- A quote and a backslash get a backslash in front.
- The control bytes `\b`, `\f`, `\n`, `\r` and `\t` use their short escape. Other control bytes use `\u00XX`.
- A valid UTF-8 sequence stays as it is.
- A byte that is not valid UTF-8 becomes `\u00XX`, the code point U+00XX. For example, the byte `0xff` becomes `\u00ff`.
- A value that is too long for the line is cut, so that the line keeps a fixed maximum size. A cut never splits an escape or a UTF-8 sequence.

A JSON line never contains a raw control byte.

### Symlink deploys

With `chdir = /srv/app/current` and `current -> releases/N` swapped atomically
(`ln -sfn` into a temporary name, then `mv -T`), the gateway resolves the
document root with `realpath()` on every request that reaches the static-file
lookup (#638): GET and HEAD for a path that is not `.php` and not a directory.
Other requests, such as POST or `.php`, do not pay for it. The next
request after the swap is served from the new release; no reload is needed.
The containment check compares against the root resolved for that same
request, so a symlink that leaves the release is still refused.

`DOCUMENT_ROOT` and `SCRIPT_FILENAME` sent to FastCGI keep the unresolved
`chdir` path (`/srv/app/current/...`). PHP and OPcache resolve and cache that
path themselves, so after a swap PHP may keep running the old release until its
realpath cache (`realpath_cache_ttl`) or its OPcache entry expires, while
static files already come from the new release. Not measured. A resolved
variant (like nginx `$realpath_root`) is not implemented. Until it is, reset
OPcache or reload the FastCGI pool in the deploy step. Also not measured: the
cost of the extra `realpath()` per request.

`http.front_controller` is checked against the document root only once, at
startup, so that check stays pinned to the release that was live then.

### What the gateway type refuses

No PHP runs in a gateway, so nothing that configures PHP applies: `pm`,
`pm.*`, `php_admin_value[]`, `php_value[]`, `request_terminate_timeout`,
`request_slowlog_timeout`, `slowlog`, `security.limit_extensions`. What stays:
`listen` (the public port), `user`/`group`, `chdir` (docroot for static files
and the front controller), `access.*`, `ping.*`, `operator.*`, `http.*`.

## URLs: local and through the gateway

Landed in #389: `http.operator = yes` builds the map described here once, in
the master at configuration time, before the first gateway forks; `fork()`
copies it into every gateway process and a reload rebuilds it.

A pool's operator pages have a **local** URL, on the operator listener, at the
path the pool declared. Through the gateway they have a **second** URL, which
the pool does not choose:

```
<gateway operator.metrics_path>/<pool name>
<gateway operator.status_path>/<pool name>
```

whatever the pool set locally. The gateway builds this map once, at
configuration time, from the pool list it already has: pool name -> (operator
listener address, local path). Two consequences:

- **Exact matches only.** `/metrics/api` is forwarded because `api` exposed
  itself; `/metrics/anything-else` is a local 404 from the gateway and is never
  forwarded. This is a guarantee, not an optimisation: the operator listener's
  own 404 lists every path it knows, which on loopback is a convenience and on
  a public port would enumerate your pools.
- **`operator.metrics = on` gives one URL, an explicit path gives two.** With
  `on`, the local path is `/metrics/<pool>` -- the same string the gateway
  uses. The only reason to write an explicit local path is a local agent that
  needs a particular one; then the pool has `/_m` locally and `/metrics/api`
  through the gateway, and you knew that when you wrote it.

The gateway's own pages sit at the bare base: `/metrics` is the gateway's own
series, `/status` its own page. On this type both paths **default to being
set**, so a fresh gateway binds the operator listener on loopback without being
asked. Set either to `""` to turn it off; that logs a warning and disables it,
and disables the `<base>/<pool>` forwarding for that format with it, because
there is no base to forward under. `http.operator = yes` with both bases empty
is a configuration error. Publicly nothing is exposed until `http.operator =
yes`, which is the switch that matters.

Because those two paths default, two gateways with no `operator.*_listen` both
land on `127.0.0.1:9253`. That still starts (issue #388): the first gateway to
register a default path keeps it, and a later one whose *derived* `/status` or
`/metrics` would collide drops that page with a NOTICE rather than refusing the
whole configuration. An **explicit** path is not offered in that way -- an
explicit collision is still a startup error, as for any pool. An explicit
`operator.status = off` / `operator.metrics = off` is honoured too and
suppresses only the default.

Sharing the default is limited to one master. A second **master** on the same
host cannot bind `127.0.0.1:9253` again and fails to start; give it its own
`operator.*_listen` or turn its pages off (see "Two masters on one host" in
`operator-endpoint.md`, issue #561).

Two gateways with `http.operator = yes` expose the same set of pools, each
under its own base. To keep one gateway out of it, turn its `http.operator` off
or empty its base paths. To keep one *pool* out of it, do not expose the pool.
There is no per-gateway pool list; if one is ever needed it is a list, not a
flag.

Membership is declared by the pool -- exposed iff it set an operator path or
flag (#273, point 4) -- and is not inferred from `http.route[]`. The gateway
forwards only the operator pages the target pool exposed; routing an application
request does not implicitly expose its status or metrics.

## What the gateway forwards with

The operator listener speaks HTTP/1.1, so the forwarding uses the same client
transport #344 adds for `http-direct` targets. One transport, two uses: a
target pool's request listener, and the operator listener. Nothing new is
invented for it.

## Absolute-form request targets

RFC 9112 3.2.2 obliges a server to accept `GET http://host/path HTTP/1.1`. The
gateway reduces such a target to origin-form (`/path`, or `/` when empty) once
and uses that for `ping.path`, the operator namespace and its ACL,
`access.suppress_path[]`, the plain-HTTP redirect and both transports; the
authority (without userinfo) replaces the `Host` header, so `HTTP_HOST`,
`SERVER_NAME` and the `Host` sent to an `http.route[]` target agree (#534).
Routing and static lookups already used the parsed path. `http:/path` is read the same way, and the forwarded target (`REQUEST_URI`,
the request line to an `http.route[]` target, the redirect `Location`) is built
from that same parse. A target that starts with `/` is always origin-form: `//api/users` is the path
`//api/users`, not host `api` plus `/users`. An authority longer than 261 bytes is answered 400.

## The upstream's `Status:` header

The gateway turns the upstream's CGI `Status:` header into the HTTP status line.
Only `NNN` or `NNN reason` with a final status (200..599) is accepted, the same
range `http-direct` uses. Anything else (`abc`, `-5`, `99999`, a 1xx, an empty
value) makes the gateway answer `502 Bad Gateway`, log a WARNING
(`upstream sent invalid Status`), record 502 in the access log and drop the rest
of the upstream reply (#594).

## An upstream that fails after the response head

Once the status line and headers are on the wire the gateway can no longer
answer 502. If the upstream then dies or breaks its framing (a FastCGI
connection closed without `END_REQUEST`, an HTTP target closed before the
chunked terminator or the full `Content-Length`), the gateway logs a WARNING
(`failed after the response head was sent`), writes the access-log line with the
status already sent, and closes the client connection **without** the
terminating chunk. The client sees an incomplete message instead of a complete
one, and a cache in front does not store the truncated body. A body that is
delimited by the upstream closing its connection ends normally. An HTTP/1.0
client gets a close-delimited reply, which no close can mark as incomplete
(#533).

Body bytes the gateway has read but not yet written to a slow client are
**dropped**, not delivered: the close is immediate and there is no drain. The
access-log byte count is reduced by the unsent output buffer, so the line
reports what left the process rather than what the gateway had read (#635).
The subtraction is exact for a `Content-Length` or close-delimited reply; a
chunked reply's count can be low by the framing of the chunks still pending,
because that framing sits in the same output buffer as the body.
## An upstream that ends its reply inside the response head

The same rule one step earlier. The CGI header block is complete only when its
blank line arrives, so an upstream that stops before it -- a `fastcgi` worker
killed between writing header lines, a target that closed the connection, a
FastCGI application that sent `END_REQUEST` without ever closing its block --
has not answered, only started to. The gateway answers **502 Bad Gateway**, the
same answer an upstream with no answer at all gets, and the partial block is
dropped rather than forwarded: it is unterminated, its last line may be half a
header, and there is no telling where the head stops and the body starts.

The WARNING names how much was buffered (`upstream '<address>' ended its reply
after N bytes of an unterminated CGI header block; answering 502`), the access
log records 502, and `fpmng_gateway_requests_total{target=...}` counts the
request as any other routed request -- it counts requests, not successes.

It is **not** the connection abort described above, and the reason is exactly
what that section says: an abort is the answer when the reply has already
started, because the only thing left to send would be the terminator, and
sending it would make a truncated body look complete. Here nothing of the
reply is on the wire yet, so a 502 is still truthful and costs the client one
retry instead of a reseted connection. Measured on 2026-10-06 with
`fpmng-http-gateway-upstream-partial-head.phpt`: the client gets libevent's
complete `502` page (`Connection: close`, as for every other gateway-generated
502, not `Transfer-Encoding: chunked`), while the same reply before the fix was
`200 OK` with the unfinished header block **as the body**:

```
HTTP/1.1 200 OK
Transfer-Encoding: chunked
...
2a
Status: 200 OK
Content-Type: text/plain

0
```

An `http.route[]` HTTP target never reaches this branch -- its parser holds an
unfinished head in its own buffer, so `fpm_http_start_reply()` is not reached,
`c->cgi_headers` stays empty and the no-answer 502 answers it. That transport
logs the truncation itself (`upstream '<address>' closed in the middle of the
response head`) before the same 502 (#463, transport #462). The two transports
now agree, which is what the FastCGI branch above was the odd one out about
(#636).

A header block above `FPM_HTTP_MAX_CGI_HEADERS` (64 KiB) with no blank line in
sight is a different case and keeps its own answer: `fpm_http_stdout()` gives up
on it being a header block and hands the buffer to the body as-is, so it is
served as a `200` whose body is the block. Measured on 2026-10-06: a 120016-byte
block in two `FCGI_STDOUT` records came back as `HTTP/1.1 200 OK` with the whole
block as the body. It is the same class of problem as this section and wants
the same decision taken on purpose; it is not changed here.

## Upstream status counters on keep-alive

The gateway always sends `FCGI_KEEP_CONN`, so a `fastcgi` upstream hits the
upstream FPM keep-alive counting bug (php/php-src#18956): `max active processes`
reads too high and `idle`/`active` can lag by up to one heartbeat. See
[operator-endpoint.md](operator-endpoint.md#known-upstream-bug-keep-alive-counters-phpphp-src18956).

## The gateway's own numbers

Gateway processes are not workers and have no scoreboard slot. Their numbers
live in **one shared-memory segment per gateway pool** that the master
allocates in `.init_main`, before the first fork. It holds two kinds of data,
and the difference is the point:

- **Pool-wide monotonic counters** -- the baseline `requests` and ping totals,
  the per-target request/rejection counts, and the paused-response total
  (issue #706). Every gateway process bumps them
  with cmp-set atomics and no locking, and they **survive a respawned gateway
  process**: the segment belongs to the pool, not the process.
- **Per gateway process gauges** -- `connections_open`, `responses_paused`
  (issue #706) and the per-target `upstreams_used`. Each process writes only
  its own block, the renderer
  **sums** every block (the #333 live-gauges shape), and the master **zeroes a
  dead process's block** in `fpm_http_gateway_on_exit()`. A gauge is "currently
  open", and a process killed with connections open runs no close callback, so
  a single shared gauge could only ever leak; per process, its connections
  leave the sum with it. The master also returns that process's upstream
  reservations to the shared admission budget, so a crash does not shrink the
  pool's budget for the life of the segment. (A process killed inside the one
  instruction between reserving the shared budget and publishing its own gauge
  can leak a single reservation; the ordering fails closed rather than
  over-spending.)

The operator child renders all of it; it is not a gateway process, so it reads
shared memory and configuration only.

`fpmng_pool_requests_total{pool="<gw>"}` is the pool's **baseline counter** --
the `requests` key the status page reports -- bumped for every request the
gateway accepts, before the ACL, so a denied request still counts. Both public
listeners feed it: the TLS one and `http.plain_listen`, whose redirects, ACME
HTTP-01 answers, NO_CERT 503s and 400s are all local. So the baseline always
equals the sum of the target rows below. Its own series label a **target**:

| series | `target` | counts |
|---|---|---|
| `fpmng_gateway_requests_total` | a routed pool | requests routed to that target |
| `fpmng_gateway_rejected_total` | a routed pool | of those, 503s from a full target (the #341 series) |
| `fpmng_gateway_upstreams_used` | a routed pool | persistent connections currently held to it |
| `fpmng_gateway_upstreams_max` | a routed pool | the target's own `pm.max_children` |
| `fpmng_gateway_requests_total` | `operator` | operator pages forwarded through `http.operator` (#389) |
| `fpmng_gateway_requests_total` | `-` | requests the gateway answered itself (ping, static, ACME, 404, 403) |
| `fpmng_gateway_request_duration_seconds` (histogram) | a routed pool, `operator` or `-` | the duration of each answered request, see below |

`fpmng_gateway_connections_open{pool="<gw>"}` and
`fpmng_gateway_ping_total{pool="<gw>"}` are the numbers no target owns: the
first is the per-process sum described above, the second a pool-wide counter.
`fpmng_gateway_responses_paused{pool="<gw>"}` and
`fpmng_gateway_responses_paused_total{pool="<gw>"}` (issue #706) count the flow
control of `http.response_buffer`. The gauge is the paused responses of all
gateway processes, summed as above. The counter rises by one each time a
response pauses its upstream. Neither series has a `target` label, because a
pause is counted for the whole pool.

`fpmng_gateway_request_duration_seconds` (issue #652) is a Prometheus histogram for each `target`. The `le` buckets are the Prometheus client defaults: 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5 and 10 seconds, and `+Inf`. Each target has a `_bucket` series for each `le`, a `_sum` series in seconds, and a `_count` series. The `_count` series equals the `+Inf` bucket.

The duration starts when the gateway reads the whole request. The duration ends when the gateway finishes the response. The gateway finishes the response when the target sends its whole answer. The duration includes the wait for a free connection to the target. A paused upstream delays the end, so a slow client can make a duration longer. The duration does not wait for the client to read the bytes that are still in the output buffer.

The histogram counts a request once, when its response finishes. A client that closes the connection early counts in `fpmng_gateway_requests_total`, but not in the histogram. The access log has no line for such a request either. The `-` target also counts the ping answers and the answers of the plain listener (`http.plain_listen`).

`fpmng_gateway_requests_total` counts a request when it arrives. So a request that is still running is in the counter, but not yet in the histogram. When no request is running, `_count` equals `fpmng_gateway_requests_total` for each target, except for a client that closed early.

The `/metrics` page also carries an **index**: one
`fpmng_gateway_exposed_pool{pool="<pool>",metrics="<base>/<pool>",status="<base>/<pool>"} 1`
line per pool the gateway forwards for (#389), so a scraper that found the
gateway knows where `<base>/<pool>` points. It is a discovery aid, not an
aggregate of their series; that endpoint was removed in #278 and stays removed.
`/status` on the gateway is the same numbers as JSON, one row per target plus a
pool row, in the generic `{"pools":[...]}` shape. The pool row also has the keys
`responses_paused` and `responses_paused_total` (issue #706).

Only the monotonic counters survive a respawned gateway; the gauges are
reconciled when a process dies, and the whole segment is rebuilt by a reload:
an exec-reload re-execs the master and the allocation is `MAP_ANONYMOUS`
(#330), so like every other pool's counters, the gateway's reset on reload.

### Client-index scaling measurement (issue #490)

`build/benchmark-gateway-client-index.py` measures the cost of keeping idle
keep-alive clients out of the lookup path. On 2026-09-25 00:27 UTC it ran on
the test box with one gateway, `http.reuseport = off`, both gateway timeouts
set to `0`, and local `/ping`; the configured application was never called.
For each connection count the harness opened that many clients, kept the oldest
socket, warmed it with 50 requests, then recorded three batches of 500
sequential pings. It then performed three rounds of up to 500 close/reopen
operations. Each arm ran twice, once in each order, and the table averages all
six ping batches plus the two setup/churn totals.

Before is commit `91a254c`; after is the #490 working-tree build. Both used
php-src `php-8.5.9` resolved to `dd6e76cce27aaa0ed9f7520648ed1081dfb6af36`,
gcc 15.2.0, libevent 2.1.12-stable, Python 3.14.4, and Linux
`7.0.0-31-generic`. Binary SHA-256 values were
`6232bbd195acb34f3959e6f57700c03ac582f63ca4a854b5c3e88e5b167883b1`
and `6b6b428e2a2cecb7f6f05b003bb3fb9238641069b63816d75e6d017e39599364`.
The harness verifies the gateway marker with `strings` before every run and
records the complete configuration and raw per-request samples in its JSON
output.

| live clients | before mean ms | after mean ms | before p95 ms | after p95 ms | setup before/after ms | churn before/after ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.127 | 0.128 | 0.151 | 0.150 | 0.5 / 0.5 | 0.8 / 0.8 |
| 1,000 | 0.148 | 0.127 | 0.174 | 0.150 | 223.6 / 215.4 | 375.9 / 345.7 |
| 5,000 | 0.227 | 0.160 | 0.301 | 0.194 | 1,334.7 / 1,148.3 | 498.3 / 347.4 |
| 10,000 | 0.330 | 0.129 | 0.382 | 0.154 | 3,266.6 / 2,258.3 | 705.1 / 346.8 |

The setup and churn columns include Python socket setup and loopback network
time, so they are end-to-end costs rather than an isolated C timing. At 10,000
idle clients the after-arm mean stayed within 0.0011 ms of its one-client mean;
the before-arm mean was 2.56 times the after-arm mean, and the after-arm p95 was
60% lower.
These are local scaling measurements, not a production capacity claim.

## A complete configuration

```ini
[global]
pid = /run/php-fpm-ng.pid
error_log = /var/log/php-fpm-ng/error.log
log_level = notice

; ===========================================================================
; THE GATEWAY -- a pure proxy. No PHP, no process manager, no workers.
; Every route is explicit: there is no implicit "own pool at /".
; ===========================================================================
[gw]
pool.type = gateway
user = www-data
group = www-data
listen = 0.0.0.0:443
chdir = /srv/www/public                 ; docroot for static files + front controller

http.route[app] = /                     ; key = pool, value = prefix list
http.route[api] = /api,/v2/api

http.tls_cert = /etc/ssl/site.crt
http.tls_key  = /etc/ssl/site.key
http.static = yes
http.front_controller = index.php
http.access_log = /var/log/php-fpm-ng/access.log
http.max_body = 16M

ping.path = /ping                       ; answered IN the gateway, before routing.
ping.response = pong                    ; Proves the gateway lives -- NOT the targets.

; The gateway's own operator pages. On this type both DEFAULT to the values
; below; written out only for clarity. "" turns one off (with a warning).
operator.metrics_path = /metrics
operator.status_path  = /status
; operator.*_listen unset -> 127.0.0.1:9253, shared with every pool below
; (9253 is the Prometheus registry's PHP-FPM exporter port; moves off 8080 in #386)

; Expose the operator pages through this public port. For every pool that
; exposed itself the gateway serves <base>/<pool name>, regardless of the
; pool's local path. Exact matches from a map built at config time; anything
; else is a local 404, never a forward.
http.operator = yes
http.operator_allowed_clients = 10.0.0.0/8

; ===========================================================================
; The application. An ordinary upstream FastCGI pool; it does not know
; HTTP exists.
; ===========================================================================
[app]
pool.type = fastcgi
user = www-data
group = www-data
listen = /run/php-fpm-ng/app.sock
listen.owner = www-data
listen.mode = 0660
chdir = /srv/www/public
pm = dynamic
pm.max_children = 32
pm.start_servers = 8
pm.min_spare_servers = 4
pm.max_spare_servers = 12
pm.max_requests = 500
php_admin_value[memory_limit] = 256M

ping.path = /ping                       ; upstream meaning: on this pool's own socket
pm.status_path = /_fpm_status           ; upstream meaning: on this pool's own socket, for nginx

operator.metrics = on                   ; -> local /metrics/app, identical to the gateway URL
operator.status  = on                   ; -> local /status/app

; ===========================================================================
; A pool that speaks HTTP itself. Routed to over HTTP/1.1 (#344).
; Explicit local paths here, to show the other form.
; ===========================================================================
[api]
pool.type = http-direct
user = www-data
group = www-data
listen = 127.0.0.1:9000
chdir = /srv/www/api/public
pm = static
pm.max_children = 16
http.front_controller = index.php
http.max_connections = 256

ping.path = /ping                       ; on this pool's own listener, in the child

operator.metrics_path = /_m              ; deliberate: short path for a local agent.
operator.status_path  = /_s              ; Through the gateway these are STILL
                                         ; /metrics/api and /status/api.

; ===========================================================================
; Long-lived connections. NOT routed through the gateway -- see below.
; ===========================================================================
[ws]
pool.type = http-direct
pool.executor = worker
user = www-data
group = www-data
listen = 0.0.0.0:8443
pm = static
pm.max_children = 4
worker.max_pending = 1024
worker.request_timeout = 30
worker.max_memory = 512M
worker.max_lifetime = 12h

operator.metrics = on
operator.status  = on                   ; reduced page: no per-request stage (#387)
ping.path = /ping                       ; 503 while the worker's queue is full (#387)

; ===========================================================================
; No HTTP endpoint of their own at all. The operator listener is their ONLY
; exposition, which is exactly why the gateway forwarding matters here.
; ===========================================================================
[cronjobs]
pool.type = cron
user = www-data
group = www-data
cron.schedule = */5 * * * *
cron.script = /srv/www/app/bin/tick.php
cron.timeout = 4m
cron.timezone = Europe/Warsaw
cron.expect_within = 10m
cron.output_log = /var/log/php-fpm-ng/cron.log

operator.metrics = on
operator.status  = on

[queue]
pool.type = supervisor
user = www-data
group = www-data
supervisor.script = /srv/www/app/bin/worker.php
supervisor.processes = 4
supervisor.restart = always
supervisor.restart_delay = 2s
supervisor.restart_delay_max = 60s
supervisor.max_memory = 256M
supervisor.output_log = /var/log/php-fpm-ng/queue.log

operator.metrics = on
operator.status  = on
```

### What is reachable where

On the public port `443`, through the gateway (operator paths only from
`10.0.0.0/8`):

```
/  /api  /v2/api                        -> app, api
/ping                                   -> the gateway itself, locally
/metrics  /status                       -> the gateway's own series and page
/metrics/app       /status/app
/metrics/api       /status/api          <- although the pool's local paths are /_m and /_s
/metrics/ws        /status/ws           <- status is the reduced worker page (#387)
/metrics/cronjobs  /status/cronjobs
/metrics/queue     /status/queue
```

On the loopback operator listener, one process, the **local** paths:

```
/metrics  /status                       (gw)
/metrics/app  /status/app               (operator.metrics = on -> same URLs as via the gateway)
/_m  /_s                                (api -- the one place the URL differs)
/metrics/ws
/metrics/cronjobs  /status/cronjobs
/metrics/queue  /status/queue
```

Besides: `8443` for `[ws]`, and `app.sock` for an nginx that might stand
beside the gateway, with `/ping` and `/_fpm_status` in their upstream meaning.

### Three things the example shows that are easy to forget

- **`[ws]` is not routed through the gateway.** WebSockets cannot be carried
  over FastCGI at all, and #344's HTTP/1.1 client is not a passthrough either;
  #343 does them natively on the worker executor. That pool has its own public
  port, and it is the first place where "the gateway is the entry point" is
  not the whole truth.
- **`[app]` has status under two names and it is not a duplicate.**
  `pm.status_path` is the page on the FastCGI socket for nginx;
  `operator.status = on` is the page on the operator listener. Two sockets,
  two answering processes, two different sets of numbers. Under the old names
  this could not be expressed at all -- which is what the rename buys.
- **`[cronjobs]` sets `cron.expect_within`.** The master checks stale-enabled
  cron pools once per second, so the `WARNING` fires even without a scrape
  (#357). The `stale`/`stale_since` page fields still appear when you scrape
  `/metrics/cronjobs` or `/status/cronjobs`; the gateway forwards them like any
  other operator page.

## Worker-executor operator pages and ping (issue #387)

`pool.type = http-direct` with `pool.executor = worker` supports `ping.path` and
`operator.status` even though it has no per-request scoreboard stage, duration
or CPU accounting. `access.log` / `access.format` remain refused because they
need per-request timing; the reduced status page reports only what this
executor measures honestly.

- **`ping.path` is answered by the worker on its request listener.** It is a
  literal path match in the connection handler, requires no PHP or scoreboard
  read, and is checked after the ACL and saturation gate. A worker whose pending
  queue is full answers `503` on the ping path too: ping means "would this
  worker accept a request now?" Pings do not consume `pm.max_requests`.
- **The operator status page is reduced**, showing pool-level answered-request
  counters, `worker_pending`, `worker_watchers`, HTTP-direct totals and per-child
  rows without request stage, duration, CPU or peak memory.
- **Gateway forwarding works for both pages.** When the pool exposes
  `operator.status_path`/`operator.status` or `operator.metrics_path`/
  `operator.metrics`, `http.operator = yes` can forward them under the gateway's
  `<base>/<pool name>` URL just like any other exposed target.

Thus the gateway-first topology has a ping on each request listener and status
and metrics on operator listeners; the pages describe the process that owns
them, not a synthetic aggregate inferred from `http.route[]`.

## Related decisions

These issues established the gateway and operator-endpoint contract documented
above; they are implementation history, not pending work:

| Issue | Decision or feature |
| --- | --- |
| #340 | Explicit `http.route[<pool>]` path-prefix routing |
| #382 | Gateway answers its own `ping.path` before routing |
| #344 | HTTP/1.1 client transport used for routed HTTP-direct pools and operator forwarding |
| #386 | `operator.*` namespace, shorthand flags, and path-safe pool names |
| #387 | Worker executor ping and reduced status page |
| #388 | Separate `pool.type = gateway`; retire the combined `http` type |
| #389 | Optional `<base>/<pool>` operator-page forwarding through the gateway |
| #390 | Gateway shared-memory counters and exposed-pool metrics index |
| #383 | FastCGI pools may opt into the shared operator listener while retaining upstream `pm.status_path` |
