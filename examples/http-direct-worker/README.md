# amphp on an FPM worker that owns its event loop

Task 073 POC. Not a supported feature — see
`docs/http-direct-revolt-integration.md` for the design and its limits.

There is a second example next door: `examples/http-direct-worker-mysql/`
(task 074) runs the same driver but sleeps in MySQL over `amphp/mysql`, which
is what actually exercises the fd watchers on a socket. This one only ever
touches the timer primitive.

## What this shows

`pool.type = http-direct` + `pool.executor = worker` boots **one** PHP script
per worker for the worker's whole lifetime and lets that script drive the
worker's libevent base itself. That is enough for **unmodified**
`revolt/event-loop` to run on it, and therefore for the whole `amphp/*`
ecosystem: `Amp\delay()`, `Amp\async()`, `amphp/http-client`, `amphp/redis`.

The HTTP server, the process manager, reload, the scoreboard and the log
plumbing stay FPM's. The application is `app.php` — 20 lines, one route.

## The pieces

| File | Role |
|---|---|
| `FpmngDriver.php` | A Revolt `AbstractDriver` over the `fpmng_worker_*` primitives. ~90 lines, all userland. |
| `FpmngServer.php` | Glue: one readable watcher on the notify pipe, one fiber per request, drain-then-stop on `SIGQUIT`. |
| `app.php` | The application: hello world plus `/sleep`, which does `Amp\delay(1.0)`. |

The SAPI knows nothing about Revolt or amphp; it exposes fd/timer watchers, a
one-iteration loop pump and a request queue. `FpmngDriver.php` is therefore
also the completeness proof task 072 asked for: if a stock Revolt driver can
be written against the primitives, the primitives are sufficient.

## Running it

```sh
composer install
```

```ini
[amphp]
listen = 127.0.0.1:8080
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1          ; on purpose: prove concurrency inside ONE process
chdir = /path/to/examples/http-direct-worker
http.front_controller = /app.php
http.read_timeout = 30000
http.max_body = 1M
catch_workers_output = yes   ; the worker's echo/stderr goes to the FPM log
php_admin_value[max_execution_time] = 0
```

```sh
curl localhost:8080/                       # hello world from pid 1234
time (for i in $(seq 8); do curl -s "localhost:8080/sleep?id=$i" & done; wait)
# ~1 s, not 8 s — one worker, eight suspended fibers
```

`build/test-http-direct-amphp.sh` automates exactly the above.

## Why `max_execution_time` must be 0

There is one `php_request_startup()` per worker, so the Zend timeout would
apply to the *worker's lifetime*, not to one HTTP request: it would kill a
healthy worker mid-service. The master refuses any other value rather than
leaving it silently unenforced.

## Logging from a worker

`STDIN`, `STDOUT` and `STDERR` are registered for the worker script (issue
#73), as they are for `pool.type = supervisor` and `pool.type = cron` (issue
#126). They are the CLI SAPI's constants and nothing else in an FPM process
registers them, so before that they did not exist here and
`fwrite(STDERR, ...)` was `Uncaught Error: Undefined constant "STDERR"` — which
mattered most in the one place it appears in `FpmngServer.php`, the
`catch (\Throwable)` inside `Amp\async()`: the `Error` escaped the fiber into
Revolt's uncaught-throwable handler and one failing request killed the worker
instead of logging a line.

Where the three descriptors point is decided by FPM, not by the worker: stdin
is the `/dev/null` the master installs, so `STDIN` reads EOF at once, and
stdout and stderr are the pipes to the master under `catch_workers_output =
yes` and `/dev/null` without it. `echo` takes the same route as `STDERR` — the
SAPI writes worker output straight to the process's stderr, because one
`php_request_startup()` covers the whole worker and there is no per-request
output buffer to route it into.

They are registered once per worker rather than once per request, which is the
same rule the script-running pools follow rather than an exception to it: a
worker has exactly one PHP request, spanning its whole life.

For a line that carries a severity and reaches the error log with no
`catch_workers_output` at all, use `error_log()` — in this SAPI it goes through
the pool's own log channel with the severity derived from the error level
(issue #124).

## Limits you will hit immediately

- **No per-request isolation.** One PHP request context per worker, so
  superglobals, `header()` and `echo` do not belong to a request. A handler
  *returns* `[status, headers, body]`; `echo` goes to the worker's stderr.
- **One blocking call stalls every request in the worker.** `PDO`, `file_get_contents()`
  on a network path, `sleep()` — anything not built on Revolt blocks all
  in-flight fibers. Use `amphp/*` clients, or use classic `http-direct`.
- **No signal watchers.** The FPM master owns `SIGQUIT`/`SIGUSR2`;
  `EventLoop::onSignal()` throws `UnsupportedFeatureException`.
- **Every accepted request must be answered.** A pending request is released
  only by `fpmng_worker_respond()`; there is no per-request timeout, because
  this executor rejects `request_terminate_timeout`. A handler that returns
  without responding burns a slot for good, and after 256 such requests the
  transport answers 503 and asks the worker to stop so the master respawns it —
  a burst of more than 256 genuinely concurrent requests recycles the worker
  the same way. `FpmngServer` answers from a `finally` for that reason.
- **A bridge decides it may exit with `fpmng_worker_may_exit()`, never with an
  in-flight counter of its own.** The counter only sees what
  `fpmng_worker_next_request()` already handed over, and the SAPI has a queue
  behind it. A request can land in that queue during the very loop iteration in
  which the last in-flight response trips `pm.max_requests` — which
  `fpmng_worker_respond()` does itself, so this needs no signal — and a bridge
  that trusts its counter then tears the loop down and closes that connection
  with no response (task 080). `fpmng_worker_may_exit()` is true only when the
  stop was requested *and* nothing accepted is still unanswered, which is why
  `FpmngServer` keeps no counter. `fpmng_worker_stopping()` remains the earlier,
  different question — "is a shutdown under way", worth knowing when you want
  to close pools or flush metrics — and it is *not* a licence to exit.
  Not accepting is not the application's job either: the transport answers 503
  itself from the moment the stop is requested.
- **Response headers are validated.** A header name that is not an RFC 9110
  token, or a header set that exceeds 64 KB, makes `fpmng_worker_respond()`
  return `false` and sends a 500 — the header is never silently dropped.
- **No TLS, no streaming responses, no HTTP/2, no per-request scoreboard
  accounting.**
