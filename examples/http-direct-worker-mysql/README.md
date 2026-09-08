# amphp/mysql on an FPM worker that owns its event loop

Experimental. `pool.executor = worker` is a POC (task 073), not a supported
feature. This example is task 074: the same worker, but the sleeping route
sleeps in **MySQL** instead of in a timer.

## Why a second example

`examples/http-direct-worker/` proves the loop with `Amp\delay(1.0)`. That
touches exactly one primitive — `FPMNG_WORKER_TIMER`. This one touches the
rest, which is the whole reason it exists:

| Route | Primitive it exercises | Why it matters |
|---|---|---|
| `/` | none (plain response) | Sanity: the worker booted and answers. |
| `/mysql` | `FPMNG_WORKER_WRITE` then `FPMNG_WORKER_READ` **on a TCP socket** | An async `stream_socket_client()` first waits for writability — a primitive no other test registers. Then the MySQL protocol reads. Before this example, the only descriptor anything had watched was the notify pipe. |
| `/mysql-tls` | the same, on a **TLS** stream | `fpmng_worker_event_create()` arms the watcher on the raw fd from `php_stream_cast()`. A stream that buffers above the descriptor — TLS, filters — can hold bytes the fd never reports as readable; task 079 made the loop re-check those buffers. Measured: it does not bite here — see below. |

Both MySQL routes run `SELECT SLEEP(1)`. The sleep happens **on the server**,
so it cannot be faked by a timer on our side: if N concurrent requests come
back in about one second from one worker, then N sockets really were in flight
inside a single PHP process.

## Running it

You need Docker and an **already built** `php-fpm-ng`. Nothing else — no PHP on
the host, no `composer install`: Composer runs inside the image, because our
own binary is built with `--disable-all` and has no phar.

A static binary works everywhere; a dynamically linked one works too, as long
as the runtime stage has its shared libraries. `build/static-full.sh` runs
*inside* Alpine and installs its own build dependencies with `apk`, so it is
invoked through Docker, never on the host — this is the same command the
`static-musl` CI job runs:

```sh
git clone --depth 1 -b php-8.5.9 https://github.com/php/php-src php-src
./build/prepare.sh "$PWD/php-src"
mkdir -p build-static out-static
docker run --rm \
  -v "$PWD/php-src:/src" -v "$PWD/build-static:/build" \
  -v "$PWD:/repo" -v "$PWD/out-static:/out" \
  alpine:3.22 sh /repo/build/static-full.sh

cp out-static/php-fpm-ng-full examples/http-direct-worker-mysql/php-fpm-ng

cd examples/http-direct-worker-mysql
docker compose up --build --wait
```

```sh
curl 127.0.0.1:28078/
curl 127.0.0.1:28078/mysql?id=1
curl 127.0.0.1:28078/mysql-tls?id=1
```

```sh
docker compose down --volumes
```

The whole stack is its own Compose project with its own volume, so the
database is created and destroyed with the demo; no shared MySQL is touched.

Or let the harness do all of it, including the measurement:

```sh
./build/test-http-direct-worker-mysql.sh /path/to/php-fpm-ng
```

It skips cleanly (exit 0) if Docker or the Compose plugin is missing. It uses
its own Compose project name (`fpmng-worker-mysql-harness-$$`) and its own port
(28088, not the example's 28078) on purpose: its exit trap runs
`down --volumes`, and on a shared box that must not be able to reach a stack
started by hand or by a second run.

## What was measured

Binary confirmed before measuring (`strings`, one hit each):
`php-fpm-ng/http-direct-worker`, `fpmng_worker_builtins`, `filter_var`,
`ctype_digit`.

Both routes work, including TLS. `mysql:8.4.6`, `amphp/mysql 3.1.1`,
one worker, `pm.max_children = 1`:

| Route | N | Wall time | `t0` spread | Overlap | Worker pid | Server-side `Ssl_cipher` |
|---|---|---|---|---|---|---|
| `/mysql` | 8 | ~1 s | 17.4 ms | 1.006 s | one (7) | empty |
| `/mysql-tls` | 8 | ~1 s | 13.0 ms | 0.992 s | one (7) | `TLS_AES_256_GCM_SHA384` |
| `/mysql` | 32 | ~1 s | 63.2 ms | 0.970 s | one (7) | empty |
| `/mysql-tls` | 32 | ~1 s | 46.9 ms | 0.960 s | one (7) | `TLS_AES_256_GCM_SHA384` |

"Overlap" is `min(t1) - max(t0)` from the worker's own clock: the last query
started that long before the first one finished. Serialized execution would
give a negative number.

Raw per-request evidence, 8 concurrent requests to `/mysql`, all `"pid":7`
(`slept` is 0 because that is what `SLEEP()` returns when it slept the whole
duration):

```
{"id":"1","mode":"plain","pid":7,"slept":0,"cipher":"","t0":1788872795.965597,"t1":1788872796.988887}
{"id":"2","mode":"plain","pid":7,"slept":0,"cipher":"","t0":1788872795.975581,"t1":1788872796.988419}
{"id":"3","mode":"plain","pid":7,"slept":0,"cipher":"","t0":1788872795.980514,"t1":1788872796.988723}
{"id":"4","mode":"plain","pid":7,"slept":0,"cipher":"","t0":1788872795.965135,"t1":1788872796.988786}
{"id":"5","mode":"plain","pid":7,"slept":0,"cipher":"","t0":1788872795.977167,"t1":1788872796.988838}
{"id":"6","mode":"plain","pid":7,"slept":0,"cipher":"","t0":1788872795.982962,"t1":1788872796.988985}
{"id":"7","mode":"plain","pid":7,"slept":0,"cipher":"","t0":1788872795.975601,"t1":1788872796.988555}
{"id":"8","mode":"plain","pid":7,"slept":0,"cipher":"","t0":1788872795.982944,"t1":1788872796.988933}
```

Eight `SELECT SLEEP(1)` in one second, not eight seconds, from one pid. The
same shape on `/mysql-tls`, with the server confirming the session is
encrypted:

```
{"id":"1","mode":"tls","pid":7,"slept":0,"cipher":"TLS_AES_256_GCM_SHA384","t0":1788872797.014846,"t1":1788872798.019604}
{"id":"2","mode":"tls","pid":7,"slept":0,"cipher":"TLS_AES_256_GCM_SHA384","t0":1788872797.017627,"t1":1788872798.021810}
```

### The TLS answer: the `php_stream_cast()` warning did not bite

The route table above flags a real risk: the watcher is armed on the raw
descriptor, so a stream that buffers above it could hold bytes the fd never
reports as readable, and the request would hang. **It does not happen here.**
32 concurrent queries over TLS finish in the same ~1 s as plain TCP.

The reason is that amphp never trusts the descriptor first. `read()` does a
**direct read before arming any watcher**, and only enables the readability
callback when that read comes back empty
(`vendor/amphp/byte-stream/src/ReadableResourceStream.php:186-207`). The
library says so in its own comment on line 186:

```php
// Attempt a direct read because PHP may buffer data, e.g. in TLS buffers.
```

So bytes sitting in the TLS buffer are consumed by the read itself, and the fd
watcher is only ever used to wait for genuinely new data. A client written this way sidesteps the buffered-stream trap entirely; a client
that waits for readability *first* and reads once per event used to hang, which
is what task 079 fixed in `fpmng_worker_loop()`. amphp's trick is now available
generically as `fpmng_worker_stream_has_buffered()` — see
`docs/http-direct-revolt-integration.md`, "Buffered streams".

The connections really are encrypted, and that is checked on every run rather
than trusted. It has to be: amphp asks for TLS by setting `CLIENT_SSL`, but if
the server does not advertise the capability the bit is masked off and the
connection **continues in plaintext with no error**
(`vendor/amphp/mysql/src/Internal/ConnectionProcessor.php:1556-1572`). A
`/mysql-tls` that had quietly become a second copy of `/mysql` would still
overlap, still say `"mode":"tls"`, and would prove nothing about a buffered
stream.

So the sleep query reads the server's own view of its session in the same
statement, and therefore on the same pooled connection:

```sql
SELECT SLEEP(1) AS slept,
       (SELECT VARIABLE_VALUE FROM performance_schema.session_status
        WHERE VARIABLE_NAME = 'Ssl_cipher') AS cipher
```

The harness fails the TLS batch if any `cipher` is empty, and fails the plain
batch if any is set. Both directions of that gate were checked against crafted
responses before being relied on.

### The build needs `ext-filter` and `ext-ctype`

Found the hard way, and fixed in `build/static-full.sh` as part of this
example. Under `--disable-all` neither is built, and neither absence is
reported at startup — you get an exception from inside a library instead:

- without `ext-filter`, `league/uri-interfaces` (a hard `ext-filter`
  dependency, reached via `amphp/socket`) fails on
  `filter_var($host, FILTER_VALIDATE_IP)`, and every connection attempt turns
  into `Error: Invalid URI: tcp://mysql:3306`;
- without `ext-ctype`, `amphp/dns` → `daverandom/libdns` cannot parse the name
  to resolve, so hostname lookup is what fails next.

Any `--disable-all` build meant to run amphp needs both.

## The pieces

| File | Role |
|---|---|
| `app.php` | The application: hello world, `/mysql`, `/mysql-tls`. One pool per mode, built on first use. |
| `compose.yaml` | Pinned `mysql:8.4.6` plus the app container. `--max-connections=500`, because `SELECT SLEEP(1)` holds a server thread for the whole second — but see the caps below, the server limit is not the first one you hit. |
| `Dockerfile` | Composer stage plus a runtime stage that takes the prebuilt binary. Build context is `examples/`, and the layout inside is fixed, so `APP_DIR` is the only thing task 075 has to change to reuse it. |
| `fpm.conf` | One `pool.executor = worker` pool, `pm.max_children = 1`. |

The Revolt driver is **not** duplicated here: `app.php` requires
`../http-direct-worker/FpmngDriver.php`. The driver is the thing under test, and
a second copy would drift.

## Why `pm.max_children = 1`

Any concurrency number from more than one worker would prove nothing that stock
FPM does not already do. One worker is the whole claim.

## Why `clear_env = no`

The application reads `MYSQL_HOST` and friends from the container environment.
FPM clears the environment by default, which would silently leave it connecting
to `127.0.0.1`.

## Limits

Everything in `examples/http-direct-worker/README.md` still applies — no
per-request isolation, one blocking call stalls every in-flight request, no
signal watchers, no TLS on the *listener*, no streaming, no HTTP/2. On top of
that:

- **Not a database guide.** Pool sizing, prepared statements, transactions and
  reconnection policy are deliberately absent.
- **The pool is per worker.** With `pm.max_children > 1` each worker would open
  its own pool, so the connection count multiplies by the number of workers.
- **Three caps, in the order they bite.** The client pool
  (`MYSQL_POOL_MAX`, default 100 — amphp's own default too), then
  `FPM_WORKER_PENDING_MAX = 256`
  (`sapi/fpmng/fpm/fpm_http_direct_worker.c:73`), beyond which the worker asks
  to be recycled, and only then the server's `max_connections`. The harness
  raises `MYSQL_POOL_MAX` to N; above ~256 concurrent requests this example
  stops being a measurement of the loop.
- **Peer verification is off** on the TLS route: the certificate is the one
  `mysqld` generates for itself. That is fine for measuring an event loop and
  wrong for anything else.
