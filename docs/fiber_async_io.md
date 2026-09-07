# Fiber: what is non-blocking, and what blocks the whole process

State as of 2026-09-07, `pool.executor = fiber`. Applies to `pool.type = fastcgi-ng`
and `pool.type = http`.

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

Measured (task 005, test box, one worker): 4 parallel requests each fetching
an `https://` endpoint that sleeps 500 ms finished in 0.524–0.533 s total
(4 × 0.5 s serialized would be 2.0 s); the same against MySQL with
`REQUIRE SSL` (`SELECT SLEEP(0.5)` per request) finished in 0.523–0.527 s.
Each response was checked to carry its own marker, not another request's.
Control: `pool.executor = classic` with 4 children serves the same 4 TLS
requests correctly (each child blocks its own), 0.523–0.527 s.

Measured: 4 parallel requests with `fsockopen()` to a server answering after
500 ms finished in 508 ms in one process. As a control, 4 x `usleep(500 ms)`
took 2009 ms.

## Blocks the whole process

- `sleep()`, `usleep()`, `time_nanosleep()`;
- **curl** — its own sockets, outside the streams layer;
- **libpq**, i.e. `pdo_pgsql` and `pgsql`;
- **TLS without the fiber build**: `ssl://`, `tls://`, `https://` are only
  non-blocking when the binary was built with `--enable-fpmng-fiber` and
  ext/openssl static (`HAVE_FPMNG_FIBER_TLS`). With a shared `openssl.so`
  the interception is off and TLS blocks as before — configure prints a
  warning in that case;
- DNS (`getaddrinfo` in connect) — see below, in progress;
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
- Code that polls multiple streams at once uses `stream_select()`, which
  **still blocks today** — it's on the list to intercept and it's cheap
  (swap the function handler, suspend instead of `select()`).

### The caveat is gone: this now pays off for TLS too

Since task 005 the stream handler is concurrent for `https://` as well —
the `ssl` transport is intercepted by patch 0007. The advice "use Guzzle on
streams" now covers HTTPS endpoints, managed databases that require TLS, and
Redis over TLS.

Swapping `ext-curl` for the stream handler cannot be forced by our
`zend_disable_functions` — Guzzle checks `extension_loaded('curl')`, and
disable_functions does not touch that. This has to be an application decision.

## Order of work

1. **DNS** — in progress (branch `fiber-async-dns`). Cheapest of the important
   ones and fixes a hole in what already works: connecting to a host BY NAME
   blocks the process for the duration of `getaddrinfo`, so concurrent MySQL
   by hostname today is not fully concurrent.
2. ~~**TLS**~~ — done (task 005, patch 0007).
3. `sleep`/`usleep` and `stream_select` — cheap, swap the function handlers.
4. **curl** — only once 1-3 are done and coverage is still missing.
5. `pdo_pgsql`/libpq — out of reach without a patch to `ext/pdo_pgsql`; PDO
   calls the synchronous `PQexec`, so there is nowhere to hook in.

## Overriding note

This list is about I/O CONCURRENCY. It changes nothing about redeclaration:
an application with `require vendor/autoload.php` still fails on the second
request ("Cannot redeclare class ComposerAutoloaderInit..."), because
`included_files` is per request, while the function and class tables are per
process. These two things are independent, and async has nothing to run on
until that one is fixed. See [fiber_errors.md](fiber_errors.md).
