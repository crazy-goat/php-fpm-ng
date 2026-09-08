# 074 — Example: worker-mode HTTP-direct + amphp/mysql, runnable with Docker Compose

Status: done
Type: example + measurement
Depends on: 073 (merged)
Related: 075 (ReactPHP on the same primitives), `docs/http-direct-revolt-integration.md`

## Why

Task 073 proved that unmodified `revolt/event-loop` drives an FPM worker's
libevent base, but it proved it with `Amp\delay(1.0)` only. That exercises
exactly one primitive: `FPMNG_WORKER_TIMER`. Two things therefore remain
unproven:

1. **fd watchers on a TCP socket.** The only descriptor any test has watched so
   far is the notify pipe — `sapi/fpmng/tests/fpmng-http-direct-worker.phpt`
   and `examples/http-direct-worker/FpmngDriver.php` both register a single
   `FPMNG_WORKER_READ` on it. `FPMNG_WORKER_WRITE` is registered by no test at
   all, and an asynchronous `stream_socket_client()` — which is how every
   amphp/ReactPHP client opens a connection — starts by waiting for
   writability.
2. **Streams that buffer above the descriptor.** `fpmng_worker_event_create()`
   takes a raw fd out of the stream via `php_stream_cast()` and warns in place
   that a stream with userland buffering (filters, TLS) can hold bytes the
   descriptor will never report as readable. Nobody has tested whether that
   warning is theoretical or fatal.

A real async database client answers both: `SELECT SLEEP(1)` over
`amphp/mysql` is a TCP socket, a real protocol, readability *and* writability,
and a third-party library we do not control. If N concurrent `SELECT SLEEP(1)`
finish in about one second on one worker, the primitives work for real I/O and
not only for timers.

The example also has to be runnable by someone who is not us. Today the amphp
harness needs a `vendor/` tree built on another machine, because Composer
cannot run against our own `--disable-all` CLI ("PHP's phar extension is
missing", measured on the test box during task 073). Docker Compose removes
that: Composer runs in its own image, MySQL is a pinned service, and the demo
is one command.

## Scope

A second example under `examples/`, using the **same** userland Revolt driver
as `examples/http-direct-worker/` — imported, not copied, so the two cannot
diverge — plus:

- a `compose.yaml` bringing up a pinned MySQL and the application container;
- a `Dockerfile` that takes an **already built** `php-fpm-ng` and installs the
  PHP dependencies in a separate Composer stage, so no phar is needed in our
  own binary. Written so that task 075 can reuse it for a ReactPHP application
  instead of duplicating it;
- one route doing `SELECT SLEEP(1)` through `amphp/mysql`, and one doing the
  same **over TLS**;
- a harness under `build/` that measures both and skips cleanly where Docker
  is unavailable.

## Acceptance criteria

- N concurrent requests to the plain-MySQL route, each running
  `SELECT SLEEP(1)`, complete in roughly one second in total rather than N
  seconds, served by a single worker with `pm.max_children = 1`, and the
  measurement records the worker pid and per-request start/end timestamps.
- The TLS route is **measured**, and the result is written down whichever way
  it goes: if `php_stream_cast()` on a TLS stream makes the watcher miss
  buffered bytes, the failure mode and the observed symptom are recorded, the
  route is documented as broken, and a follow-up task is filed. A hang that is
  merely suspected is not an acceptable outcome.
- `docker compose up` in the example directory serves both routes, with no
  `vendor/` tree prepared by hand and no PHP on the host.
- The harness skips cleanly (exit 0) where Docker or the Compose plugin is
  missing, and never touches a shared MySQL: its own Compose project, its own
  port, torn down with its volumes.
- The example README states which primitive each route exercises, so the value
  of the example is not folded into "it works".

## Out of scope

- Any change to `sapi/fpmng/`. If the measurement finds a missing or broken
  primitive, that is a finding and a new task, not a fix bundled here.
- Connection-pool sizing, prepared-statement caching, transactions,
  reconnection policy — anything that turns the example into a database guide.
- Wiring the harness into CI. It needs Docker and pulls images; whether CI
  should gate on it is the open question already recorded in `findings.md`
  from task 073.
- ReactPHP. Same primitives, own task (075), own PR.

## Outcome

Both claims the example exists to test hold. Async fd watchers work for real
socket I/O, and they work over TLS too — which was the open question.

### What was done

- `examples/http-direct-worker-mysql/` — `compose.yaml` (pinned `mysql:8.4.6`
  plus the app), a `Dockerfile` with a Composer stage and a runtime stage that
  takes an already-built `php-fpm-ng`, `fpm.conf` with one
  `pool.executor = worker` pool at `pm.max_children = 1`, and `app.php` with
  `/`, `/mysql` and `/mysql-tls`.
- The Revolt driver is **imported, not copied**: `app.php` requires
  `../http-direct-worker/FpmngDriver.php` and `../http-direct-worker/FpmngServer.php`,
  so the two examples cannot drift.
- The `Dockerfile` takes `ARG APP_DIR` and keeps a fixed internal layout, so
  task 075 reuses it by changing that one argument.
- `build/test-http-direct-worker-mysql.sh` — brings the stack up, fans out N
  concurrent requests per route, asserts one pid, the time budget and the
  overlap invariant (`last_start < first_end`), scans the logs, and tears the
  stack down with its volumes. SKIPs (exit 0) without Docker or the Compose
  plugin. A per-run Compose project name (`fpmng-worker-mysql-harness-$$`) and
  its own port (28088, not the example's 28078): its trap runs
  `down --volumes`, and on a shared box that must not be able to reach a stack
  someone else started.
- `build/static-full.sh` — added `--enable-filter --enable-ctype`, with the
  measurement that forced it in a comment at the configure call. See below.

### What was measured

Binary confirmed before measuring (`strings`, one hit each):
`php-fpm-ng/http-direct-worker`, `fpmng_worker_builtins`, `filter_var`,
`ctype_digit`. arm64 static-pie, php-8.5.9, `amphp/mysql 3.1.1`,
`mysql:8.4.6`, one worker at `pm.max_children = 1`.

| Route | N | Wall time | `t0` spread | Overlap | Worker pid | Server-side `Ssl_cipher` |
|---|---|---|---|---|---|---|
| `/mysql` | 8 | ~1 s | 17.4 ms | 1.006 s | one (7) | empty |
| `/mysql-tls` | 8 | ~1 s | 13.0 ms | 0.992 s | one (7) | `TLS_AES_256_GCM_SHA384` |
| `/mysql` | 32 | ~1 s | 63.2 ms | 0.970 s | one (7) | empty |
| `/mysql-tls` | 32 | ~1 s | 46.9 ms | 0.960 s | one (7) | `TLS_AES_256_GCM_SHA384` |

Overlap is `min(t1) - max(t0)` from the worker's own clock. Raw per-request
evidence is in the example README. Eight `SELECT SLEEP(1)` from one pid: `t0`
1788872795.965135 .. .982962, every `t1` about 1.000 s later. The sleep runs on
the server, so N sockets really were in flight in one PHP process.

**TLS route: measured, and it works.** The acceptance criterion allowed for
either answer; the answer is that the `php_stream_cast()` warning does not
bite here, and the reason is in the client, not in us: `read()` does a direct
read before arming any watcher and only enables the readability callback when
that read returns empty
(`vendor/amphp/byte-stream/src/ReadableResourceStream.php:186-207`, with the
library's own comment "Attempt a direct read because PHP may buffer data, e.g.
in TLS buffers."). The warning in `fpmng_worker_event_create()` stays correct
as written — a client that waits for readability first and reads once per
event would still hang. No follow-up task is filed, because nothing is broken;
the caveat is documented in the example README instead.

The TLS route is genuinely encrypted, and this is asserted on every run rather
than trusted once. It has to be: amphp sets `CLIENT_SSL`, but if the server
does not advertise the capability the bit is masked off and the connection
continues in plaintext with no error at all
(`vendor/amphp/mysql/src/Internal/ConnectionProcessor.php:1556-1572`) — a
`/mysql-tls` that had silently become a second `/mysql` would still overlap and
still report `"mode":"tls"`. So the sleep statement also selects
`performance_schema.session_status` for `Ssl_cipher`, on the same pooled
connection, and the harness fails the TLS batch if any row's cipher is empty
and the plain batch if any is set. Both directions of the gate were exercised
against crafted payloads: `ERROR /mysql-tls ran in plaintext: Ssl_cipher empty
on 2 of 2 sessions` and `ERROR /mysql was encrypted (Ssl_cipher
'TLS_AES_256_GCM_SHA384'); the two routes are not distinct`.

Two caveats about the gate, both recorded rather than fixed: a failing TLS
batch is reported and **not** fatal (the acceptance criterion is the plain
route), so this harness is not a regression gate for the TLS claim — the line
to read is `concurrent-mysql-tls`, not `PASS`. And the negative case could not
be produced end to end by starting `mysqld` with TLS off: with
`--tls-version=` the server logs `Failed to set up SSL because of the following
SSL library error: TLS version is invalid` and then never becomes healthy,
because `caching_sha2_password` cannot authenticate root over an unencrypted
socket. That is why the gate was verified at the assertion instead.

### The build change, and why it is here rather than in a follow-up task

`ext-filter` and `ext-ctype` are hard `ext-*` requirements of the async client
stack, and `--disable-all` builds neither. Neither absence is reported at
startup; both surface as an exception from inside a library:

- `league/uri-interfaces` (`"ext-filter": "*"`, reached through
  `amphp/socket`) calls `filter_var($host, FILTER_VALIDATE_IP)` in
  `UriString.php:711`, so every connection attempt became
  `Error: Invalid URI: tcp://mysql:3306`
  (`vendor/amphp/socket/src/Internal/functions.php:40`);
- `amphp/dns` depends on `daverandom/libdns` (`"ext-ctype": "*"`) to parse the
  name to resolve, so hostname lookup fails next.

Verified against the built binary with `strings`: `json_encode` and
`openssl_encrypt` (the other two hard requirements, from `amphp/dns` and
`amphp/socket`) were already present; `filter_var` and `ctype_digit` were
absent. This is a two-flag change to `build/static-full.sh`, not a change to
`sapi/fpmng/` — which is what this task placed out of scope — and without it
the acceptance criterion "`docker compose up` serves both routes" cannot be
met at all.

### Blocked on, and unblocked by, task 076

The first run of this example segfaulted every worker child. Root cause was
not in the example: worker builtins were registered with a NULL `module`
pointer, and opcache's optimizer dereferenced it while constant-folding
`function_exists('fpmng_worker_loop')` in `FpmngDriver::isSupported()`. That
was filed and fixed as task 076 (merged, `8f03215`) rather than worked around
here with `opcache.enable = 0`; this branch contains no such workaround.

### Review findings fixed before the PR

A `review` subagent on the branch diff (workflow.md step 5) found five issues
worth acting on; all are fixed here and the harness was re-run afterwards
(`PASS` at N=8 and N=32, both routes, ciphers as tabled above).

1. **The TLS route could have been plaintext and the harness would still have
   printed `ok`.** The `Ssl_cipher` assertion above is the fix. This was the
   serious one: the recorded claim rested on a one-off manual query that no
   rerun reproduced.
2. **The exit trap could destroy state that was not the harness's own.**
   `compose.yaml` fixes `name: fpmng-worker-mysql`, and the harness passed no
   `-p`, so `down --volumes` in its trap reached any stack with that name —
   including one started by hand per the README, or a concurrent run of the
   harness itself. Now a per-run project name and a distinct default port.
   Separately, `rm -f "$EXAMPLE/php-fpm-ng"` ran unconditionally, including on
   the SKIP paths that return before the `cp`, so on a machine without Docker
   it deleted the operator's own binary at exactly the path the README tells
   them to use. Now guarded by a `COPIED` flag, and the same-file case is
   detected instead of being a `cp` onto itself.
3. **The MySQL healthcheck never expanded its password.** `["CMD", ...,
   "-p$${MYSQL_ROOT_PASSWORD}"]` is exec form, so nothing expanded it, and
   `mysqladmin ping` exits 0 even on `ER_ACCESS_DENIED` — `service_healthy`
   only meant "the server completed a handshake". Now `CMD-SHELL`. This turned
   out to be load-bearing: it is what made the TLS-off experiment above
   correctly report the container as unhealthy.
4. **`--max-connections=500` did not buy what its comment claimed.** The first
   cap is the client pool, which defaulted to 100
   (`vendor/amphp/sql-common/src/SqlCommonConnectionPool.php:37`); above it the
   pool queues and the overlap assertion fails for a client-side reason,
   reported as "serialized" — exactly the false blame the comment said had been
   avoided. `app.php` now takes `MYSQL_POOL_MAX`, the harness sets it to N, and
   both the comment and the README name the real order of the three caps
   (pool, then `FPM_WORKER_PENDING_MAX = 256`, then the server).
5. **`.dockerignore` was inert.** The build context is `examples/`, so BuildKit
   looks for `http-direct-worker-mysql/Dockerfile.dockerignore`. Renamed. Not a
   correctness bug — the Dockerfile copies only named files — but the file was
   giving false assurance that the local `vendor/` was excluded.

Also fixed from the same review: the `docker compose` invocation was a string
that depended on word splitting (now a shell function, so a path with a space
cannot silently retarget it), `trap ... INT TERM` captured `$?` as 0 so Ctrl-C
exited 0, and the `static-full.sh` comment credited `ext-filter` only to
`league/uri-interfaces` when `amphp/dns` requires it directly too.

The review's verdicts on `findings.md` are recorded there: two entries are
task-worthy and new (`static-full.sh` has no extension guard; the
`fpmng-supervisor-restart.phpt` `restarts=0` flake, to be investigated before
any timeout is touched), and three restate entries already confirmed under task
073 rather than warranting a second task file.

The `tls-reload` entry needed neither a task nor the review's agreement: **task
048 already covers it**, and my own note on it was wrong. I had recorded that
the job "passes both stated acceptance criteria and then fails a later check",
on a pre-existing keep-alive connection. Neither half holds.
`build/test-http-tls-reload.sh:186` prints the `info` line for criterion 1
*before* asserting it, so the `connection 0 served serial ...` failure **is**
criterion 1 failing, not something after it; and `served_serial()` opens a
fresh `openssl s_client` per sample (`:69`), so no connection survives the
swap. What is left is exactly task 048's diagnosis: a gateway that had not
ticked within the fixed `sleep 4` at `:180`. The `findings.md` entry is
corrected to say so.

### Left out

- Not wired into CI — needs Docker and pulls images. Still the open question
  recorded in `findings.md` from task 073.
- Peer verification is off on the TLS route (`mysqld`'s self-signed
  certificate). Fine for measuring an event loop, documented as wrong for
  anything else.
- Pool sizing, prepared statements, transactions and reconnection policy:
  deliberately absent, as scoped.
- ReactPHP: task 075.
