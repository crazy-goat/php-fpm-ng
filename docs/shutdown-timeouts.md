# Shutdown timeouts — operator reference

This is the operator-facing reference for how long work gets to finish when
php-fpm-ng stops: what each timeout directive does, which one wins, and what
the stock defaults actually do on `docker stop` or `systemctl stop`. For
measurements and internal rationale, see `docs/NOTES.md` (graceful stopping,
section 3p).

## Who receives the signal

`docker stop`, systemd, and most orchestrators send `SIGTERM` to **PID 1** —
the FPM **master** — not to individual pool children. The master then asks
each child to stop and, if a child is still alive after
`process_control_timeout`, escalates to `SIGKILL` itself
(`fpm_process_ctl.c`, reference code — upstream FPM behaviour, unchanged).

Sending `SIGTERM` directly to a child (unusual outside manual debugging)
follows different rules: `supervisor` and `cron` can finish or skip work
through their own handlers; request-serving pools drain the in-flight request
through the normal PHP shutdown path.

## Directives by pool type

| pool type | pool-level limit | global limit on `docker stop` | hard cap (pool type) |
|---|---|---|---|
| `fastcgi`, `http` | `request_terminate_timeout` (per request; default 0 = none) | `process_control_timeout` (master escalation after `SIGTERM` to the master) | `request_terminate_timeout` when set |
| `http-direct` with `pool.executor = worker` | none for the booted worker script; `worker.request_timeout` only bounds individual unanswered requests | reload sends SIGQUIT as a cooperative stop request; after `process_control_timeout` the master sends SIGTERM, then SIGKILL 1s later if still alive. Master termination sends SIGTERM immediately, then SIGKILL after `process_control_timeout` | global `process_control_timeout`; no worker-specific grace |
| `gateway` | none for the proxy itself; `http.write_timeout` only closes a client that stops making progress on a response | reload and stop send SIGQUIT: the gateway stops accepting, closes idle keep-alive connections and finishes requests already dispatched to a worker; a request whose body is still being uploaded is cut (see below); the master `SIGKILL`s whatever is still alive after `process_control_timeout` (not a gateway with `http.ready_path`, see below) | `process_control_timeout` |
| `supervisor` | `supervisor.stop_timeout` (default 10s) — our watchdog after `supervisor.stop_signal` (default `SIGTERM`, issue #324) to the child | `process_control_timeout` must be **≥** `supervisor.stop_timeout` or the master kills the child first | `supervisor.stop_timeout` |
| `cron` | `cron.timeout` (default 0 = no limit on a running script) — the master sends `cron.stop_signal` (default `SIGTERM`, issue #325) to the child first | same: `process_control_timeout` must be **≥** `cron.timeout` when `cron.timeout > 0`, or the master wins | `cron.timeout` when set |
| `status` | none (no PHP work to finish) | `process_control_timeout` only | none |

`process_control_timeout` is a **`[global]`** directive. It applies to every
pool in the file. `supervisor.stop_timeout`, `cron.timeout`, and
`request_terminate_timeout` are **per-pool**.

A `gateway` pool is the one request-serving type whose stop grace is
`process_control_timeout` alone (issue #641). A gateway runs no PHP: the work
in flight is a proxy exchange between the client and a target worker, and the
gateway itself has no per-request timeout to reuse. On stop and on reload the
master sends the gateway `SIGQUIT`; the gateway stops accepting, closes every
connection that is idle between keep-alive requests and lets in-flight requests
finish, exiting a moment before the deadline. The master waits
`process_control_timeout` and then `SIGKILL`s whatever is still alive, so a
client that never reads is cut at the deadline and the reload still finishes.
With the stock `process_control_timeout = 0` the gateway is killed at once,
exactly as the `SIGTERM` it replaces did; set `process_control_timeout` to get
a graceful gateway stop.

A gateway with `http.ready_path` first gets `SIGUSR1` at the start of the stop
or the reload, before any child is signalled. It then keeps serving for a
window that ends at `process_control_timeout` minus 100 ms. The probe answers
`503 draining` during that window. The children of the pools that this gateway
routes to also keep serving, so the application answers new connections. An
ondemand pool forks a child for a new connection in the window.

After the window the gateway drains for up to another `process_control_timeout`
minus 100 ms. It stops accepting, and it exits when no request is in progress.
The children held back for it get their stop signal when the gateway exits, or
at `process_control_timeout` at the latest. The master then escalates after
another `process_control_timeout`. A stop or a reload of such a pool therefore
takes up to about twice `process_control_timeout`. With the stock `0` there is no
window and no child is held back. See `docs/gateway.md`, "Readiness probe".

**What "in flight" does not include (phase-1 limitation).** A request whose
body has not finished arriving is not drained. The gateway's HTTP library
buffers a whole body in memory before it proxies it (up to `http.max_body`,
which caps that body but does not cause the buffering), so while a client is
still uploading the request has not reached a target worker: the drain does not
count the connection as in flight and the gateway exits at once. And even if the
connection were held, it could not finish, because the master stops the target
pool's workers before it drains the gateways (`fpm_pctl_action_next()` signals
every child; `fpm_pctl_exec()` → `fpm_http_cleanup()` drains the gateways
afterwards), so there is no worker left when the upload finally dispatches.
Draining an upload therefore needs the gateway drained *before* the target
workers are stopped, which phase 1 does not do;
`fpmng-gateway-drain-upload.phpt` pins the current behaviour. Requests that
reached a worker before the signal (a slow response, an SSE stream) are drained
normally.

For `pool.executor = worker`, `fpmng_worker_stopping()` is a notification to the
booted PHP script, not an interrupt: the bridge must check it and wind down its
event loop. `fpmng_worker_may_exit()` also waits until the SAPI has no pending
replies, but neither function makes userland code check itself. If a script
ignores the notification, reload remains bounded by the master: SIGQUIT is
followed by SIGTERM after `process_control_timeout`; if the worker is still
alive, the master's final SIGKILL follows one second later. A master termination
starts with SIGTERM and escalates to SIGKILL after `process_control_timeout`.
The worker executor adds no separate script-level timeout;
`worker.request_timeout` only applies to individual unanswered requests. A
script can install its own SIGTERM handler, so the final master SIGKILL is the
hard bound.

`supervisor.max_runtime` (issue #326; see
[`supervisor.md`](supervisor.md#supervisormax_runtime-a-cap-on-a-single-iteration-issue-326))
is not in this table: it is not a shutdown timeout, it never fires because the
pool is being stopped. It caps how long ONE iteration of `supervisor.script`
may run while the pool is otherwise healthy, and when it fires it reuses
`supervisor.stop_timeout`'s own watchdog (`SIGTERM`/`stop_signal`, then
`SIGKILL`) as its fallback — so a `supervisor.max_runtime` overrun still needs
the same `supervisor.stop_timeout` grace period to actually end the process,
and is subject to the same "the master's escalation on `docker stop` can win
first" caveat this page describes, if the two happen to be running at once.

## What the default combination does on `docker stop`

Stock settings: `process_control_timeout = 0` (escalate to `SIGKILL` after
about one second), `supervisor.stop_timeout = 10s`, `cron.timeout = 0`,
`request_terminate_timeout = 0`.

| pool type | child state | what happens |
|---|---|---|
| request-serving | handling a request | the master sends `SIGTERM`, then `SIGKILL` after ~1s if the request is still running — same as upstream FPM |
| `gateway` | proxying a request | the master sends `SIGQUIT`, then `SIGKILL` at once because `process_control_timeout = 0`; set `process_control_timeout` for the drain to have time (issue #641). With `http.ready_path` the gateway serves first for a window of `process_control_timeout` minus 100 ms, then drains for as long again; the children of the pools it routes to keep serving until the gateway exits or `process_control_timeout` passes, and then get `process_control_timeout` more (issue #646) |
| `supervisor` | running a script iteration | the master kills the child after ~1s; **`supervisor.stop_timeout` never runs** because the master acts first |
| `cron` | sleeping before the next run | the child exits immediately and **skips** that scheduled run — clean, no script execution |
| `cron` | script already running | the master kills the child after ~1s; with default `cron.timeout = 0` there is no pool-level watchdog either |
| `status` | idle | stops with the master; nothing to drain |

Measured: a `supervisor` script with a 2.4s iteration did not finish on
`docker stop` with default globals; with `process_control_timeout = 5s` (≥
`supervisor.stop_timeout = 3s` in that test), the pool-level watchdog had time
to act (`docs/NOTES.md`, graceful stopping).

## Recommended configuration

Anyone who wants `supervisor` or `cron` to finish in-flight work when the
container or service stops must set **`[global] process_control_timeout`** to
at least the largest `supervisor.stop_timeout` or `cron.timeout` in the
configuration.

The combined and supervisor examples set `process_control_timeout = 15s` for
this reason (`examples/combined/fpm-ng.conf`,
`examples/supervisor/fpm-ng.conf`).

For request-serving pools, `request_terminate_timeout` remains the wall-clock
guard on a single request. Setting `php_admin_value[max_execution_time] = 0`
(as recommended in the root `README.md`) removes the per-request timer but
does not remove `request_terminate_timeout` when you configure it.

## Startup warnings

php-fpm-ng logs a **one-time warning at startup** (never on each child spawn)
when a pool's shutdown grace cannot take effect on master-driven stop:

- **`supervisor`:** when `process_control_timeout < supervisor.stop_timeout`.
  This includes the stock default (`0 < 10s`). The message names both
  directives, states that `SIGTERM`/`docker stop` to the master kills the child
  through master escalation first, and suggests setting
  `process_control_timeout >= supervisor.stop_timeout`.
- **`cron`:** when `cron.timeout > 0` **and**
  `process_control_timeout < cron.timeout`. Same shape of message.

**Why warn for `supervisor` at default but not for every `cron` pool:** the
default `supervisor.stop_timeout = 10s` is an explicit promise of shutdown
grace that `process_control_timeout = 0` silently defeats on `docker stop`.
The default `cron.timeout = 0` makes no such promise — a sleeping cron child
exits cleanly without needing grace, and warning on every cron pool with stock
settings would fire on a configuration that is otherwise normal upstream FPM
behaviour. When you set `cron.timeout`, the same mismatch warning applies.

**Why not warn on request-serving pools:** `process_control_timeout = 0` is
upstream FPM's default; request workers already document their limits through
`request_terminate_timeout`. A warning on every HTTP/FastCGI pool would train
operators to ignore warnings without adding a pool-type-specific promise.

Implementation: `fpm_pool_supervisor_init_main()` and
`fpm_pool_cron_init_main()` (`sapi/fpmng/fpm/fpm_pool_supervisor.c`,
`sapi/fpmng/fpm/fpm_pool_cron.c`).
