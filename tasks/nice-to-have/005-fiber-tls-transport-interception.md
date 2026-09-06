# 005 — Non-blocking `ssl://` / `tls://` for the fiber executor

**Track:** nice-to-have. This task only applies to `pool.executor = fiber`,
which is moving behind a build flag that is **off by default**, so a stock
binary does not contain this code at all. The `Priority:` line below is the
priority *within* the fiber track — it is not a claim against the HTTP,
cron, scheduler or proxy work, which is where the project is focused.

**Priority:** high. This is the largest remaining gap in the fiber executor's
usefulness.
**Status:** open.

## Context

`sapi/fpmng/fpm/fpm_pool_fiber_xport.c` intercepts the `tcp` and `unix` stream
transports (`php_stream_xport_register()`, installed after fork in
`fpm_pool_fiber_xport_install()`), so a connect or a read that would block
suspends the fiber instead. That is what makes `fsockopen`, mysqlnd (PDO and
mysqli), phpredis, Predis and the `http://` wrapper concurrent under
`pool.executor = fiber`.

`ssl` and `tls` are **not** intercepted. Everything that runs over TLS therefore
still blocks the whole process:

- Guzzle and Symfony's HTTP client talking to any `https://` endpoint
- managed databases that require TLS (Aiven, PlanetScale, and most cloud MySQL)
- Redis over TLS

The project's own stated goal was that "Guzzle over select, MySQL and Redis" is
90% of the value. MySQL and Redis are done. This is the remaining piece, and it
also silently limits the other two whenever the connection is encrypted.

`docs/fiber_async_io.md` records what is and is not non-blocking today, and the
caveat that the existing interception only pays off after the TLS handshake.

## Problem

Make TLS-wrapped connections suspend the fiber instead of blocking the process,
for the `fiber` executor only.

## Acceptance criteria

1. With `pool.executor = fiber`, N concurrent requests each performing an
   `https://` request to an endpoint that sleeps complete in roughly the time of
   one such request, not N times it. Measured, with the numbers recorded.
2. The same holds for a TLS-wrapped database connection.
3. Correctness under concurrency is asserted on **data**, not on timing alone:
   each concurrent request must receive its own response body, not another
   request's.
4. `pool.executor = classic` and the non-fiber pool types are provably
   unaffected — the interception must be installed only in the fiber worker
   process, as the existing `tcp`/`unix` interception is.
5. A build without ext/openssl still builds and runs; the TLS path is absent,
   not broken.
6. Failure modes are explicit. If some TLS configuration cannot be made
   non-blocking, it must be **refused with a clear message** rather than
   silently blocking — the precedent is the persistent-connection refusal in
   `fpm_pool_fiber_xport.c`, which logs once and raises a warning naming the
   reason.

## Explicitly out of scope

- `curl`. It has its own event loop and is deliberately deferred; the documented
  workaround is Guzzle's StreamHandler / Symfony's NativeHttpClient
  (`docs/fiber_async_io.md`).
- Server-side TLS. That is done — the HTTP gateway terminates TLS
  (`sapi/fpmng/fpm/fpm_http_tls.c`). This task is about outgoing connections.

## Notes and hazards

- ext/openssl replaces the `tcp` transport entry in its own MINIT. The existing
  installer already accounts for ordering — it runs after MINIT and logs whether
  the transport it captured was the generic one or something else
  (`fpm_pool_fiber_xport.c`, the `stream transports hooked` message). Whatever
  is done for TLS must be equally explicit about what it wrapped.
- A TLS handshake is not one syscall: it is a sequence of reads and writes that
  can each want more data. Suspension has to be possible **inside** the
  handshake, not only around it, or the concurrency win disappears exactly where
  it is most needed.
- Do not log connection identifiers that may embed credentials. A previous
  change nearly leaked a PDO persistent-connection key, which is built from the
  DSN including user and password.
