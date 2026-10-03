# ReactPHP on the http-direct worker loop

A second, independent consumer of the event primitives a `pool.type = http-direct`
worker with `pool.executor = worker` exposes to PHP (task 073). The first one is
`../http-direct-worker/`, which runs Revolt and amphp on fibers. This one runs
ReactPHP on promises, and shares no code with it: `React\EventLoop\LoopInterface`
and Revolt's `Driver` have no common interface, so the only thing the two
directories have in common is the SAPI builtins they call.

That is the point. `docs/http-direct-revolt-integration.md` claims "the API is
not Revolt-specific; Revolt is the test". Until this example there was no
evidence for the first half of that sentence.

## Run it

```sh
docker compose up --build            # app on http://127.0.0.1:28068
curl -s '127.0.0.1:28068/'
curl -s '127.0.0.1:28068/mysql'      # SELECT SLEEP(1), one connection per request
curl -s '127.0.0.1:28068/tls'        # 1 MiB over HTTPS from the nginx origin
curl -s '127.0.0.1:28068/ticks?n=5000'
curl -s '127.0.0.1:28068/strand?chunk=1024&keep=1'
```

The automated version, which asserts all of it against a binary you name, is
`build/test-http-direct-worker-react.sh`.

## LoopInterface, method by method

`FpmngLoop` is a transposition of ReactPHP's own `ExtEventLoop`
(react/event-loop v1.6.0), which drives libevent 2 through the `event` PECL
extension. Where this file departs from it there is a comment saying why.

| `LoopInterface` method                | SAPI primitive                                     |
| ------------------------------------- | -------------------------------------------------- |
| `addReadStream`                       | `FPMNG_WORKER_READ` watcher (`EV_READ\|EV_PERSIST`)  |
| `addWriteStream`                      | `FPMNG_WORKER_WRITE` watcher (`EV_WRITE\|EV_PERSIST`)|
| `removeReadStream`, `removeWriteStream`, `cancelTimer` | `fpmng_worker_event_free()`       |
| `addTimer`                            | `FPMNG_WORKER_TIMER` watcher, one-shot             |
| `addPeriodicTimer`                    | the same watcher, re-armed by `FpmngLoop`          |
| `futureTick`                          | no primitive — `fpmng_worker_loop(false)`          |
| `run`                                 | `fpmng_worker_loop($blocking)`                     |
| `stop`                                | `fpmng_worker_loop_break()`                        |
| `addSignal`, `removeSignal`           | none: throws `BadMethodCallException`              |

Two of those rows are the ones worth reading the code for.

`futureTick()` has no Revolt analogue, so nothing before this example ever
needed `fpmng_worker_loop(false)` for its own sake. A queued tick must run on
the next loop iteration, which means the loop may not go to sleep in libevent
waiting for a descriptor nobody is waiting on — `FpmngLoop::run()` passes
`$blocking = $this->futureTickQueue->isEmpty()`.

`addSignal()` throws rather than returning quietly. The FPM master owns
SIGQUIT/SIGUSR2 and the worker lifecycle; a userland watcher competing for them
would break graceful reload. Shutdown reaches the application through
`fpmng_worker_may_exit()` and the notification stream instead
(`FpmngReactServer::stopWhenDrained()`). Task 075 drained the SAPI queue by
hand there, because a request queued behind `fpmng_worker_next_request()` is
invisible to an in-flight counter; task 080 replaced both with
`fpmng_worker_may_exit()`, which is true only when the stop was requested and
nothing accepted is still unanswered — see
`../http-direct-worker/README.md`.

## What was measured

One worker (`pm.max_children = 1`), so every request below is served by the same
pid. Raw output of `build/test-http-direct-worker-react.sh` against a
`php-fpm-ng-full` static-pie build of php-8.5.9 on arm64:

```
hello-world: ok (hello world from pid 7)
concurrent-mysql: ok (8 requests in 1s on pid 7; t0 spread 15.5ms, overlap 0.998s)
future-tick: ok (5000 ticks in 1.8ms on pid 7, blocker 10.0s never fired)
concurrent-tls: ok (4 requests in 1s on pid 7; t0 spread 6.0ms, overlap 0.988s,
                    bytes 1048576 in 32 reads (min across the batch: 21 reads))
tls-strand(keep-alive=0): ok (1048576 body bytes in 1025 reads of 1024 in 0.006s)
tls-strand(keep-alive=1): STRANDED (stranded: 769 of 8192 body bytes after
                    1 reads of 1024, quiet for 3.0s)
```

The read counts on the TLS routes vary between runs — the origin throttles by
rate, so how much has arrived when a readable event fires is not fixed. The
byte totals and the strand numbers do not vary.

- **Concurrency is real, not interleaved output.** Eight `SELECT SLEEP(1)`
  queries finish in ~1 s total on one pid, and the assertion is an overlap
  invariant rather than a wall-clock guess: `max(t0) < min(t1)`, i.e. the last
  request started before the first one finished. Measured overlap 0.998 s of a
  1 s sleep.
- **`futureTick` does not depend on a descriptor.** 5000 nested ticks drain in
  1.9 ms *while a 10 s timer is armed*. If `run()` blocked in libevent with a
  tick pending, the ticks would have taken 10 s; the harness fails if the drain
  takes as much as half the blocker.
- **Promises, not fibers, complete a request.** Nothing in this example calls
  `Fiber::suspend()`: a handler returns a promise and the response is sent from
  its callback. Tasks 073 and 074 only ever proved the fiber path.

## The read-chunk trap on TLS streams

**Task 079 closed this trap in the SAPI; the measurements below are what
motivated it, and the `/strand` route is now a regression gate —
`build/test-http-direct-worker-react.sh` fails if it ever reports the stranding
again.** `fpmng_worker_loop()` now
re-casts every read watcher's stream before letting libevent sleep and
activates the watcher itself when the stream's userland buffer is non-empty
(see `docs/http-direct-revolt-integration.md`, "Buffered streams"). What
follows is the state of the world before that.

`fpmng_worker_event_create()` carried a warning: a stream with userland
buffering — filters, TLS — can hold bytes the descriptor never reports as
readable. Task 074 predicted that "a client that waits for readability first and
reads once per event would still hang". The `/strand` route was built to settle
that, and it does, but the real condition is narrower than the prediction.

Same 8 KiB response over the same TLS origin, one read per readable event, only
the read chunk changing:

| read chunk | result                                          |
| ---------- | ----------------------------------------------- |
| 512        | stranded, 257 of 8192 body bytes after 1 read   |
| 1024       | stranded, 769 of 8192 body bytes after 1 read   |
| 4096       | stranded, 3841 of 8192 body bytes after 1 read  |
| 8191       | stranded, 7936 of 8192 body bytes after 1 read  |
| 8192       | stranded, 7937 of 8192 body bytes after 1 read  |
| 65536      | complete, 8447 bytes in 1 read                  |

And with the same 1024-byte chunk against a 1 MiB body that the origin closes at
the end: 1 048 576 bytes in 1025 reads in 0.005 s, no strand at all.

So reading once per event is not what breaks. What breaks is reading less than
the TLS layer has already decrypted into its userland buffer, *while the peer
sends nothing further*:

- One TLS read pulls everything the kernel has and decrypts it into a PHP-side
  buffer. Whatever the application does not consume stays there, invisible to
  libevent, because the descriptor is now genuinely empty.
- With `Connection: close` the peer's FIN is itself a readable event, and with a
  1 MiB body the kernel buffer never empties in the first place — either way the
  next event arrives and the loop makes progress. That is why the 1024-byte
  chunk drains a whole megabyte.
- With `Connection: keep-alive` and a response that fits in one record burst,
  there is no further event ever. The remainder is unreachable for the life of
  the connection.

Note that matching PHP's default `chunk_size` of 8192 does not save you: at
`chunk = 8192` the first read returns the head plus 7937 body bytes and strands
the last 255. There is no chunk size that is safe in general, because the
stranded amount depends on what the peer happened to send. The rule for an
application is to read until the read comes up short, not once per event —
which after task 079 is about CPU rather than correctness, because the loop
keeps invoking the watcher until the buffer drains.
ReactPHP's own default happens to satisfy this for small responses —
`ReadableResourceStream` uses a 65536-byte chunk (react/stream v1.4.0,
`src/ReadableResourceStream.php:84`) — which is why `react/http` works here
without any of this being visible.

`react/mysql` does not use TLS at all: `CLIENT_SSL` is defined and never sent
(`Io/Constants.php:57`), so the `/mysql` route cannot exercise this and the
origin exists to give TLS somewhere real to happen. The same library also only
speaks `mysql_native_password` (`Commands/AuthenticateCommand.php:78-101`),
which MySQL 8.4 ships disabled — hence `--mysql-native-password=ON` and the
`ALTER USER` in `initdb/`.

## Logging from the worker

`FpmngReactServer::fail()` reports a rejected handler with
`fwrite(\STDERR, ...)`. `STDIN`, `STDOUT` and `STDERR` exist in this executor
since issue #73; see
[`../http-direct-worker/README.md`](../http-direct-worker/README.md#logging-from-a-worker)
for where the descriptors point and why `error_log()` is the alternative that
does not need `catch_workers_output`.

## Files

| file                        | what it is                                              |
| --------------------------- | ------------------------------------------------------- |
| `FpmngLoop.php`             | `LoopInterface` over the worker's libevent base          |
| `FpmngReactServer.php`      | request queue -> one promise per request                 |
| `app.php`                   | the routes, and the assertions' data source              |
| `compose.yaml`              | app + MySQL 8.4 + an nginx TLS origin                    |
| `origin/`                   | self-signed cert, `/slow` (rate-limited), `/burst`, `/small` |
| `fpm.conf`                  | one `[worker]` pool, `pm.max_children = 1`               |

The image is built from `../http-direct-worker-mysql/Dockerfile` with
`APP_DIR=http-direct-worker-react`.
