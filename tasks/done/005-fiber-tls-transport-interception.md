# 005 — Non-blocking `ssl://` / `tls://` for the fiber executor

**Track:** nice-to-have. This task only applies to `pool.executor = fiber`,
which is moving behind a build flag that is **off by default**, so a stock
binary does not contain this code at all. The `Priority:` line below is the
priority *within* the fiber track — it is not a claim against the HTTP,
cron, scheduler or proxy work, which is where the project is focused.

**Priority:** high. This is the largest remaining gap in the fiber executor's
usefulness.
**Status:** done.

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

## Outcome (2026-09-07)

**Done.** TLS-wrapped connections suspend the fiber instead of blocking, for
`pool.executor = fiber` only, via `patches/0007-fiber-tls-nonblocking-transports.patch`
(gated on `HAVE_FPMNG_FIBER_TLS` — defined only with `--enable-fpmng-fiber`
and a static `--with-openssl`; a shared `openssl.so` gets a configure warning
and upstream-blocking TLS).

How it works: ext/openssl's internal waits (`php_pollfd_for` in the handshake
loop and in `SSL_read`/`SSL_write` retries) are re-pointed at
`fpm_pool_fiber_wait_fd` (`fpm_fiber_tls_wait`), so suspension happens INSIDE
the handshake, not around it. `SSL_MODE_AUTO_RETRY` is enabled so an
in-progress renegotiation is re-driven by OpenSSL and `SSL_want` keeps
reporting real socket state; the reneg rate limiter is re-checked explicitly
per handshake step. The ssl/sslv3/tls/tlsv1.x transports are re-armed in the
fiber worker (`fpm_fiber_tls_xport_install`, after MINIT); the returned
streams keep upstream ops identity and are wrapped lazily on the first call
from a request fiber (`fpm_fiber_xport_wrap`, one reserved slot in the ops
map), so accepted sockets and non-fiber code are untouched. All fpmng symbols
are reached through weak references, because ext/openssl objects also link
into `cli`.

**Measured** (poligon, 8-core shared box, php-8.5.11-dev tree, one worker,
`pm = static`, `pm.max_children = 1`, `pool.type = http`,
`pool.executor = fiber`):

- criterion 1 (https): 4 concurrent requests to four TLS endpoints sleeping
  500 ms each: **0.524–0.533 s** wall (serialized would be ≥ 2.0 s). 3 runs.
- criterion 2 (TLS database): 4 concurrent requests each opening a MySQL 8.4
  connection as a `REQUIRE SSL` user (TLSv1.3) and running
  `SELECT SLEEP(0.5)`: **0.523–0.527 s**, each with a distinct
  `CONNECTION_ID()`. 2 runs.
- criterion 3 (data, not timing): every response carried its own id
  (`tls-body-A`..`tls-body-D`; MySQL conn ids 1016–1019), asserted in
  `sapi/fpmng/tests/fpmng-fiber-tls-concurrency.phpt`.
- criterion 4 (classic unaffected): `pool.executor = classic` with 4 children
  serves the same 4 TLS requests correctly, 0.523–0.527 s; the interception
  is installed only in the fiber child (`fpm_pool_fiber_xport_install` runs
  from `fpm_pool_fiber_child_main`), other executors never call it. The
  non-fiber build links and runs because all fpmng symbols in the patch are
  weak references (verified: `cli` binary has them as `w` in `nm`).
- criterion 5 (no ext/openssl): `--enable-fpmng-fiber` without
  `--with-openssl` builds and runs; `HAVE_FPMNG_FIBER_TLS` is undefined and
  no TLS code is compiled in.
- criterion 6 (explicit failure modes), measured live:
  `ssl.allow_blocking = true` on connect and server-side
  `stream_socket_server("tls://...")` from a request fiber are both refused
  with a specific `E_WARNING` naming the reason, plus a once-per-process
  `ZLOG_NOTICE` (same shape as the persistent-connection refusal).

Regression suites on the poligon: `build/run-fpmng-phpt.sh` PASS=9 of 11; the
2 failures (`fpmng-fiber-request-isolation.phpt`,
`fpmng-pool-type-fiber-matrix.phpt`) are pre-existing on any fiber build —
they lack `php_admin_value[max_execution_time] = 0` and hit the coop
validation refusal; recorded in `findings.md`, not caused by this change.

**Left out / known limits:**

- `stream_get_meta_data()` `crypto` data, session resumption, early data:
  unchanged (delegated to upstream ops).
- The `zlog` NOTICE from refusals does not reach `error_log` from a fiber
  child at all (child stderr is /dev/null and child zlog output was not
  observed for the pre-existing persistent refusal either — same mechanism);
  the `E_WARNING` is the reliable channel. Recorded in `findings.md`.
- Server-side TLS over stream transports is refused, not supported (the HTTP
  gateway terminates TLS in libevent and does not use these transports).
