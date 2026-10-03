# Routing path prefixes to other pools: `http.route[]` (issue #340)

A `pool.type = gateway` is a proxy with no PHP of its own (issue #388; the
proxy half of the retired `pool.type = http`). `http.route[]` lets one gateway
send different path prefixes to different pools, so that an API, a stream
endpoint and the rest of an application can have separate worker counts,
separate PHP settings and separate failure behaviour without a separate
listener (and a separate port, and a reverse proxy in front) for each.

```ini
[gw]
pool.type = gateway
listen = 127.0.0.1:8080
chdir = /srv/app/public
http.front_controller = /index.php
http.route[web]    = /
http.route[api]    = /api/v1,/api/v2
http.route[events] = /sse

[web]
pool.type = fastcgi
listen = 127.0.0.1:9000
pm = static
pm.max_children = 8

[api]
listen = 127.0.0.1:9001
pm = static
pm.max_children = 16

[events]
listen = 127.0.0.1:9002
pm = static
pm.max_children = 4
```

`http://127.0.0.1:8080/api/v1/users` is served by the `api` pool,
`/sse/stream` by `events`, and everything else by the `web` target named by
`http.route[web] = /`.

## The directive

```ini
http.route[<pool name>] = <prefix>[,<prefix>...]
```

**The key is the target pool and the value is its list of prefixes**, not the
other way round. That is deliberate and it is the shape the budget rules below
follow from: the thing on the left is the thing that has workers, a queue and a
limit. Writing it the other way -- one line per prefix -- would suggest that two
prefixes on the same pool are two independent things, which they are not.

- A target is a `fastcgi` pool (FastCGI) or an `http-direct` pool (the HTTP/1.1
  client transport, issue #344). Routing to another `pool.type = gateway` pool
  is a gateway in front of a gateway and is refused outright; a type with no
  request listener of its own (`cron`, `supervisor`) is refused too.
- A pool may appear once. Give it all its prefixes in one comma-separated
  value; a second `http.route[api]` line is a configuration error, not an
  addition.
- Every prefix must begin with `/`, and no prefix may appear twice across the
  whole table -- one prefix selects one target.
- The table is read once, in the master, before the first gateway process is
  forked. A reload re-reads it the same way every other directive is re-read;
  there is no hot-reconfiguration of routes in a running gateway.

All of the above is checked at configuration time, so `php-fpm-ng -t` refuses a
bad table and a typo in a pool name never becomes a per-request 502.

## How a request is matched

The gateway matches on the **decoded path** of the request URI -- the query
string plays no part, and `/api%2Fv1` is matched as `/api/v1`.

Matching is **longest prefix first** and **segment-aware**: a prefix matches a
path when the path is the prefix itself or continues it at a path separator.
With `http.route[api] = /api` :

| path         | matches `/api`? |
| ------------ | --------------- |
| `/api`       | yes             |
| `/api/`      | yes             |
| `/api/v1/x`  | yes             |
| `/apiary`    | **no**          |

`/apiary` is not below `/api`, it merely starts with the same letters. With
both `/api` and `/api/v1` in the table, `/api/v1/users` goes to whichever pool
claimed `/api/v1`: the longer prefix wins regardless of the order the lines are
written in.

## `/` is an ordinary prefix, and there is no implicit target

Issue #388 removed the old weld's implicit own-pool target. On
`pool.type = gateway` the route table is exactly `http.route[]`:

- At least one `http.route[]` entry is required. A gateway with none is a
  configuration error (`-t` refuses it), because there is no pool behind it to
  serve an unrouted request.
- `/` is a prefix like any other. On this type it must be claimed explicitly,
  as `http.route[web] = /` above; it is not inserted for you.
- A request that matches **no** prefix is answered **404 by the gateway
  itself** and is never forwarded. The access log's `target` field is `-`, the
  same marker a locally answered ping or static file gets. If no entry claims
  `/`, the gateway logs a startup NOTICE saying so -- an API-only gateway is
  legal, but it is usually not what an operator meant.

`/` is the shortest prefix, so when it is present every unmatched path falls
back to the pool that claimed it. Segment awareness still applies: `/apiary`
does not fall under `/api`.

## The budget belongs to a pool, not to a prefix

Each target has its own connection budget, sized from **that pool's**
`pm.max_children`, its own set of persistent upstream connections and its own
`http.pool_full_policy` waiting queue. Two consequences, both intended:

- Two prefixes on the same pool share one budget. With
  `http.route[api] = /api/v1,/api/v2`, filling `api` to its `pm.max_children`
  makes both prefixes answer `503` + `Retry-After` together. They are one
  resource seen through two names.
- A full target does not affect the others. If `events` is saturated, `/sse/*`
  answers 503 while `/` keeps being served at full speed. This is the main
  operational reason to route at all: a slow endpoint can no longer eat the
  worker pool the rest of the site depends on.

`http.pool_full_policy`, `http.pool_full_queue_max` and `http.pool_full_wait_ms`
are still gateway-wide settings (see
[`http-gateway-pool-full.md`](http-gateway-pool-full.md)); what is per target is
the queue they govern.

## What the targets see

The gateway builds the FastCGI request; the target pool only executes it.
`DOCUMENT_ROOT` and `SCRIPT_FILENAME` therefore come from the **gateway's**
`chdir`/docroot for every target, and so do `http.static` and
`http.front_controller`. All pools in a routed configuration must be able to see
the same files. Routing splits the *workers*, not the *code*.

What does differ per target is everything the pool itself owns: `pm`,
`pm.max_children`, `php_value`/`php_admin_value`, `env[]`, `user`/`group`,
`request_terminate_timeout`, `slowlog`.

## A note on `/sse` and long-lived requests

A stream endpoint is the obvious thing to route away from the rest of the
application, and this is a real improvement -- but it does not make streaming
cheap. Every request in flight still pins one worker of the target pool for its
whole duration, so `pm.max_children` on an `events` pool is a hard cap on
concurrent streams. Issue #179 tracks the cost of that model. Routing bounds the
damage; it does not remove it.

## Routing to `http-direct` pools (issue #344)

An `http.route[]` target may be a `pool.type = http-direct` pool. The gateway
then speaks plain HTTP/1.1 to that pool's own listener (`http.listen`) instead
of FastCGI, and the response is re-framed through the same path the FastCGI
STDOUT records use — so `http.route[/sse] = sse_pool` reaches an http-direct
pool with `http.stream = 1`, or a `pool.executor = worker` SSE stream (#342),
unchanged.

Request mapping:

- The **method and request-target arrive as received** — the route prefix is
  *not* stripped, the same rule FastCGI follows with `REQUEST_URI`. One
  consequence for document roots: the target resolves the path against its
  own docroot **including the prefix**, so with
  `http.route[direct] = /direct` the file behind `/direct/index.php` is
  `docroot/direct/index.php` — a shared docroot, exactly as for FastCGI
  targets (or the pool's own `http.front_controller` fallback).
- Input headers are copied minus the hop-by-hop set: `Connection`,
  `Keep-Alive`, `Transfer-Encoding`, `Upgrade`, `Trailer`, `Proxy-*`. The
  gateway's own `Connection: keep-alive` replaces the client's.
- **`X-Forwarded-For`** gets the address the gateway actually saw appended
  (or the header created); **`X-Forwarded-Proto`** and **`-Port`** are set
  from the gateway's resolved forwarded state. A direct pool has no
  trusted-proxy list of its own (its `REMOTE_ADDR` is its direct peer — the
  gateway), so `X-Forwarded-For` is the application's evidence that a proxy
  was involved, the same as behind nginx.
- The body travels with an explicit `Content-Length` (the gateway has the
  whole body buffered already — evhttp guarantees that before dispatch).

Response mapping: `Content-Length` is honoured as identity framing (the body
passes through byte-for-byte); a chunked upstream response is de-chunked and
re-framed by evhttp, one piece per chunk — which is what makes streaming work.
The upstream's `Connection` header is the upstream connection's business and
never reaches the client.

Failure matrix, identical to a FastCGI target's: a target that cannot be
reached or dies mid-request answers **502** with a log line naming the
target's address; a full target (the target pool's `pm.max_children` is also
the connection budget) answers **503 + `Retry-After`**, subject to the same
`http.pool_full_policy`.

Two deliberate refusals:

- **`Upgrade` is answered 501**, not stripped: a stripped websocket handshake
  hangs the client mid-handshake, which is worse than an explicit "not
  implemented". WebSocket is #343's worker-executor feature; gateway
  passthrough would be a later decision.
- A target that **terminates TLS on its own listener** (`http.tls_cert` on the
  target pool) is refused at startup: this transport speaks cleartext to
  loopback and unix sockets only.

Streaming cost, unchanged from the FastCGI case: every open stream pins one
of the target's connections (one budget slot) for its whole life.



## What is logged

At startup a routed gateway prints its table in lookup order, longest prefix
first:

```
NOTICE: [pool gw] http.route: '/api/v1' -> pool api (16 persistent connection(s))
NOTICE: [pool gw] http.route: '/sse' -> pool events (4 persistent connection(s))
NOTICE: [pool gw] http.route: '/' -> pool web (8 persistent connection(s))
```

Reading it top to bottom is reading it the way the gateway matches it.

## See also

- [`http-gateway-pool-full.md`](http-gateway-pool-full.md) -- what a full
  target does with the request.
- [`operator-endpoint.md`](operator-endpoint.md) -- status and metrics are per
  pool and are not routed; each target answers on its own operator listener.
- [`gateway.md`](gateway.md) -- the gateway type that owns this routing table.
