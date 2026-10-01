# Fiber: what is non-blocking, and what blocks the whole process

State as of 2026-09-07 (interception list and the IO seam section updated
2026-10-02, issue #531), `pool.executor = fiber`. Applies to `pool.type =
fastcgi` (since issue #388 retired `pool.type = http`; a gateway reaches such a
pool through `http.route[]`). The measurements behind this page were taken on
the retired optimized-FastCGI transport, removed in 0.9.0 (issue #376), and
have to be redone on plain `fastcgi` if fiber is ever promoted.

Thing to remember: in this executor one process serves N requests at once, so
**one blocking call stops every request in flight**, not just its own. In
classic FPM it would only slow down one request.

## Intercepted today

`fpm_pool_fiber_xport.c` replaces the `tcp` and `unix` transport factories via
`php_stream_xport_register()` and suspends the fiber on read/write/connect
instead of blocking. Task 005 extends the same to TLS: patch
`patches/0007-fiber-tls-nonblocking-transports.patch` (compiled only with
`--enable-fpmng-fiber` and a static ext/openssl — `HAVE_FPMNG_FIBER_TLS`)
suspends the fiber inside the TLS handshake and inside `SSL_read`/`SSL_write`
retries, and re-arms the `ssl`/`sslv3`/`tls`/`tlsv1.x` transports in the fiber
worker. Everything that goes through the PHP streams layer benefits:

- `fsockopen()`, `stream_socket_client()`;
- **mysqlnd**, i.e. `PDO` (mysql) and `mysqli` in the default build — and
  through them Doctrine and Eloquent, with no application code line changed;
- **phpredis** and Predis;
- the `http://` **and `https://`** wrappers (`file_get_contents`, `fopen`),
  and with them Guzzle's `StreamHandler` and Symfony's `NativeHttpClient`
  (see the curl section below);
- TLS-wrapped databases and Redis (`tls://...` DSNs, e.g. managed MySQL that
  requires TLS).

The host-name lookup inside those connects goes through evdns on the
scheduler's loop, not a blocking `getaddrinfo()`. Outside the streams layer,
four more calls suspend the fiber instead of the process:

- `sleep()`, `usleep()`, `time_nanosleep()` (`fpm_pool_fiber_sleep.c`);
- `stream_select()` (`fpm_pool_fiber_select.c` + patch 0008), except with a
  non-empty `$except` set or a zero timeout;
- `flock()` and `file_put_contents(..., LOCK_EX)` against a lock held by
  another request of the same process (`fpm_pool_fiber_flock.c`);
- ext/session's file lock between requests of one process
  (`fpm_pool_coop_session_patch.c`, the in-process arbiter).

All of them go through one internal seam; see [The IO seam](#the-io-seam).

Measured (task 005, test box, one worker): 4 parallel requests each fetching
an `https://` endpoint that sleeps 500 ms finished in 0.524–0.533 s total
(4 × 0.5 s serialized would be 2.0 s); the same against MySQL with
`REQUIRE SSL` (`SELECT SLEEP(0.5)` per request) finished in 0.523–0.527 s.
Each response was checked to carry its own marker, not another request's.
Control: `pool.executor = classic` with 4 children serves the same 4 TLS
requests correctly (each child blocks its own), 0.523–0.527 s.

Measured: 4 parallel requests with `fsockopen()` to a server answering after
500 ms finished in 508 ms in one process. As a control, 4 x `usleep(500 ms)`
took 2009 ms (measured before sleep was intercepted; this is what
`fiber.disable_interceptions = sleep` gives back today).

## Blocks the whole process

- anything named in `fiber.disable_interceptions` (see below);
- `pcntl_sleep()`, `time_sleep_until()`;
- **curl** — its own sockets, outside the streams layer;
- **libpq**, i.e. `pdo_pgsql` and `pgsql`;
- **TLS without the fiber build**: `ssl://`, `tls://`, `https://` are only
  non-blocking when the binary was built with `--enable-fpmng-fiber` and
  ext/openssl static (`HAVE_FPMNG_FIBER_TLS`). With a shared `openssl.so`
  the interception is off and TLS blocks as before — configure prints a
  warning in that case;
- DNS outside connect (`gethostbyname()`, `dns_get_record()`), and DNS in
  connect when there is no `/etc/resolv.conf` (evdns cannot start; logged);
- plain files, `ext/sockets` (`socket_*`), libmemcached.

TLS configurations that cannot be made non-blocking are **refused loudly**
(E_WARNING + failed connect, one NOTICE per process in the log), never
silently blocking: `ssl.allow_blocking => true` in the stream context, and
server-side `ssl://`/`tls://` listeners created from a request fiber (accepted
sockets would inherit the wrapper ops; terminate TLS at the HTTP gateway,
`http.tls_cert`, instead — it never went through these transports anyway).

## curl: deferred, worked around via the stream handler

Intercepting curl would require replacing the `curl_exec` handler and
rewriting the transfer onto `curl_multi` with `CURLMOPT_SOCKETFUNCTION`/
`TIMERFUNCTION` hooked into our libevent loop. In the True Async fork this
piece is `ext/curl/curl_async.c`, +2278 lines. **Deliberately deferred** —
this is a separate project and code that breaks with every change to ext/curl.

Workaround on the application side: use the HTTP client in **streaming** mode
instead of the curl one, because streams are already intercepted.

- **Guzzle** picks the curl handler by default when `ext-curl` is available.
  Forcing the stream handler (`GuzzleHttp\Handler\StreamHandler`
  passed to `HandlerStack::create()`) moves all traffic onto the PHP wrappers.
- **Symfony HttpClient** has the same split: `NativeHttpClient` (streams)
  versus `CurlHttpClient`.
- Code that polls multiple streams at once uses `stream_select()`, which is
  intercepted too (patch 0008).

### The caveat is gone: this now pays off for TLS too

Since task 005 the stream handler is concurrent for `https://` as well —
the `ssl` transport is intercepted by patch 0007. The advice "use Guzzle on
streams" now covers HTTPS endpoints, managed databases that require TLS, and
Redis over TLS.

Swapping `ext-curl` for the stream handler cannot be forced by our
`zend_disable_functions` — Guzzle checks `extension_loaded('curl')`, and
disable_functions does not touch that. This has to be an application decision.

## Order of work

1. ~~**DNS**~~ — done: a host name in a `tcp` connect is resolved through
   evdns. Before, connecting to a host BY NAME blocked the process for the
   duration of `getaddrinfo`, so concurrent MySQL by hostname was not fully
   concurrent.
2. ~~**TLS**~~ — done (task 005, patch 0007).
3. ~~`sleep`/`usleep` and `stream_select`~~ — done (swap the function
   handlers; patch 0008).
4. **curl** — the remaining gap. It plugs into the IO seam below as one more
   registry entry (`curl`); the seam itself does not change.
5. `pdo_pgsql`/libpq — out of reach without a patch to `ext/pdo_pgsql`; PDO
   calls the synchronous `PQexec`, so there is nowhere to hook in.

## The IO seam

Every interception above reaches the scheduler through one internal header,
`sapi/fpmng/fpm/fpm_pool_fiber_io.h`, shaped after the
[IO Hooks RFC](https://wiki.php.net/rfc/io_hooks) (Under Discussion, targets
PHP 8.7). No interception module and no php-src patch includes libevent or
calls the scheduler; `build/test-fiber-io-seam.sh` (CI, hermetic) fails if one
does. If the RFC lands, its provider replaces `fpm_pool_fiber_io.c` and the
modules stay.

| RFC concept | fpm-ng | Notes |
|---|---|---|
| Operation | `struct fpm_fiber_io_op_s` | one per wait; optional deadline (`timeout`) |
| `Poll` (one fd, read/write) | `FPM_FIBER_IO_OP_POLL` | stream read/write/connect, TLS waits (patch 0007) |
| `Timer` | `FPM_FIBER_IO_OP_TIMER` | `sleep()` family, `flock()` retry interval |
| `Any` (first of several) | `FPM_FIBER_IO_OP_ANY` | `stream_select()`; a member libevent cannot watch (a regular file) is reported ready, as `select()` does |
| `GetAddrInfo` | `FPM_FIBER_IO_OP_GETADDRINFO` | host name in a `tcp` connect; hints fixed to TCP stream sockets, all families |
| — (no counterpart) | `FPM_FIBER_IO_OP_WAKE` + `fpm_fiber_io_waker()`/`fpm_fiber_io_wake()` | in-process wait queues: the `flock()` registry and the session lock arbiter |
| Completion status | `enum fpm_fiber_io_completion` | |
| `Ready` / `Done` | `FPM_FIBER_IO_READY` | for POLL/ANY see `revents`; GETADDRINFO's answer may be an error (`gai_error`) |
| `Timeout` | `FPM_FIBER_IO_TIMEOUT` | never for TIMER, whose normal end is READY |
| `Cancelled` | `FPM_FIBER_IO_CANCELLED` | handled by every client (e.g. `time_nanosleep()` returns the time left), never produced by the libevent backend |
| `Unsupported` | `FPM_FIBER_IO_UNSUPPORTED` | nothing was suspended: not in a request fiber, a nested user Fiber, the interception is disabled, or no evdns. The caller makes the stock blocking call |
| `Interrupted` | — | no signal ever ends a wait early in this executor |
| Provider | `fpm_fiber_io_run()` | readiness-based, syscall-first: the caller tries the call, and waits only when it would block |

### Registry and switch

`sapi/fpmng/fpm/fpm_pool_fiber_intercept.c` holds one table of the
interceptions, in install order. The order is a constraint, not a style: the
child installs them after every extension's MINIT, and `xport` must come
after ext/openssl's, which registers its own `tcp` factory there.

| Entry | Covers | Installs |
|---|---|---|
| `xport` | `tcp`/`unix` streams, DNS in connect, TLS (patch 0007) | transport factories |
| `flock` | `flock()` / `LOCK_EX` on plain files | `php_stream_stdio_ops.set_option` |
| `sleep` | `sleep()`, `usleep()`, `time_nanosleep()` | three function handlers |
| `select` | `stream_select()` | nothing: patch 0008's call site is compiled in |

`fiber.disable_interceptions = name[, name]` (pool directive, fiber executor
only) switches entries off for one pool. An unknown name fails the start with
the list of known ones. A disabled entry is not installed, and the seam
answers UNSUPPORTED to it, which also turns off the call sites compiled in by
patches 0007 and 0008: the call behaves exactly as in stock PHP and blocks the
whole process (one NOTICE per child says so). For `flock` it is worse: an
in-process lock conflict then hangs the child for good, and a WARNING per
child says that too. It is a diagnostic and an escape
hatch for an interception that misbehaves, not a tuning knob.
`fpmng-fiber-disable-interceptions.phpt` checks it: with `sleep` disabled, two
concurrent `usleep(500 ms)` run one after the other, while `stream_select()`
in the same pool still overlaps.

The session lock arbiter uses the seam but is not in the table, so it cannot
be disabled: without it there is no stock behaviour to fall back to, only no
in-process session lock or a deadlock on ext/session's own `flock(2)`.

Adding an interception (curl) is one file with its own
`struct fpm_fiber_intercept_s` and one table line; removing one is the
reverse.

## Overriding note

This list is about I/O CONCURRENCY. It changes nothing about redeclaration:
an application with `require vendor/autoload.php` still fails on the second
request ("Cannot redeclare class ComposerAutoloaderInit..."), because
`included_files` is per request, while the function and class tables are per
process. These two things are independent, and async has nothing to run on
until that one is fixed. See [fiber_errors.md](fiber_errors.md).
