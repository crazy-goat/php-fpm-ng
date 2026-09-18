# Native WebSocket on `pool.executor = worker`

The runnable example for the [WebSocket section of
`docs/http-direct.md`](../../docs/http-direct.md) (issue #343). Dependency
free: `app.php` is the whole application, including an ~80-line RFC 6455
codec — in production that job belongs to `amphp/websocket-server` or
`ratchet/rfc6455`, running on the same stream.

## What it shows

The C side contributes exactly one builtin, `fpmng_worker_upgrade()`:

- it answers the `101 Switching Protocols` handshake (`Sec-WebSocket-Accept`
  computed in C, `Sec-WebSocket-Protocol` passed through `$responseHeaders`),
- hijacks the connection — the accepted bufferevent, the OpenSSL one on a TLS
  pool — away from evhttp, and
- returns it as an ordinary bidirectional PHP stream.

`app.php` binds a read watcher to that stream (a normal
`fpmng_worker_event_create()`), echoes text and binary frames, answers pings
with pongs, replies to close with close, and — on
`fpmng_worker_stopping()` — sends `1001 Going Away` before closing, which is
what a retiring worker owes its WebSocket clients.

While frames flow, ordinary HTTP requests to the same worker keep being
answered: one worker, one event loop.

## Running it

```ini
[ws]
listen = 127.0.0.1:8080
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = /path/to/examples/http-direct-worker-ws
http.front_controller = /app.php
http.read_timeout = 30000
http.max_body = 1M
php_admin_value[max_execution_time] = 0
```

```sh
curl -N http://localhost:8080/                    # ordinary request
# with any websocket client:
# connect ws://localhost:8080/ws, send "hello", expect "echo: hello"
```

## Limits

- **No TLS-to-client restriction** — on a TLS pool the hijacked bufferevent is
  the OpenSSL one, whole; the codec below it sees plaintext either way.
- **The gateway cannot proxy WebSocket** — `Upgrade` through `http.route[]`
  is answered 501 (issue #344); FastCGI has no way to carry the stream.
- **No permessage-deflate, no C frame codec.** A codec that proves too slow
  for a workload is a follow-up, not part of #343.
- **Read until short.** `fread()` returns everything buffered and `''` when
  none is left; a partial read strands the rest until the next frame. The
  same rule every stream on this executor obeys.
