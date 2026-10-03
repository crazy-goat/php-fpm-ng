# Server-Sent Events on `pool.executor = worker`

The runnable example for the "Server-Sent Events (SSE)" section of
[`docs/http-direct.md`](../../docs/http-direct.md) (issue #342). Dependency
free: no Composer, no amphp — the raw `fpmng_worker_*` primitives and one
`Fiber` per open stream.

## What it shows

`app.php` is a complete SSE endpoint in about 90 lines:

- `GET /events` opens a stream (`Content-Type: text/event-stream`,
  `Cache-Control: no-cache`), replays the events after the reconnecting
  client's `Last-Event-ID` header (or `?last_event_id=`), and sends a
  `: ping` comment every 15 s.
- `GET /publish?msg=...` appends one event to the worker's in-memory log;
  every open stream on **this worker** picks it up on its next heartbeat.
- Dead clients are dropped the moment the SAPI reports them through
  `fpmng_worker_closed_requests()` — not one heartbeat later, which is what
  the pre-#342 "learn it from the next `_chunk()` returning false" behaviour
  cost.
- During replay and live heartbeat updates, a `false` from `_chunk()` is checked
  against `fpmng_worker_request_env()`: a gone/reaped client ends that Fiber,
  while a live client retries the same chunk after yielding to the event loop.
  The per-chunk retry ceiling is 10 seconds. On timeout or worker stop, the
  example gives `event: bye` a separate 5-second drain window, then ends the
  response to release the pending slot (#454, #455). The stream cursor advances
  only after a chunk is queued, so heartbeats don't replay already-sent IDs.

## Running it

```ini
[sse]
listen = 127.0.0.1:8080
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = /path/to/examples/http-direct-worker-sse
http.front_controller = /app.php
http.read_timeout = 30000
http.max_body = 1M
php_admin_value[max_execution_time] = 0
```

```sh
curl -N localhost:8080/events &            # a subscriber
curl "localhost:8080/publish?msg=hello"    # -> id: 1, data: hello on the stream
```

Kill the subscriber with Ctrl-C and reopen with
`curl -N -H "Last-Event-ID: 1" localhost:8080/events` — the example replays
everything after that id.

## The three semantics, where they live

| #342 semantics | Where in `app.php` |
|---|---|
| Stream exempt from `worker.request_timeout` | nowhere — the SAPI's `fpmng_worker_respond_start()` marks it; no userland code needed |
| Clean end on worker retirement | the `fpmng_worker_stopping()` check, `_end()` after a final `event: bye` |
| Client gone reported by id | the `fpmng_worker_closed_requests()` drain in the notify watcher |

## Limits

- **Fan-out is per worker.** With `pm.max_children = 1` that is the whole
  pool; with more children, each child has its own event log and a subscriber
  sees only what was published to its own worker. Cross-worker
  publish/subscribe is issue #182's question and deliberately not answered
  here.
- **Polling heartbeat.** Streams discover new events on their 15 s timer. A
  real application would want a cross-worker wakeup primitive (also #182) or
  a Revolt/amphp transport (the neighbouring `examples/http-direct-worker/`).
- **One `worker.max_pending` slot per open stream.** Size the directive to
  your expected subscriber count, and see `worker.send_buffer_limit` for the
  slow-reader lever.
