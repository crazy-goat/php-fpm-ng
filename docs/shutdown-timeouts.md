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
| `fastcgi`, `fastcgi-ng`, `http` | `request_terminate_timeout` (per request; default 0 = none) | `process_control_timeout` (master escalation after `SIGTERM` to the master) | `request_terminate_timeout` when set |
| `supervisor` | `supervisor.stop_timeout` (default 10s) — our watchdog after `supervisor.stop_signal` (default `SIGTERM`, issue #324) to the child | `process_control_timeout` must be **≥** `supervisor.stop_timeout` or the master kills the child first | `supervisor.stop_timeout` |
| `cron` | `cron.timeout` (default 0 = no limit on a running script) — the master sends `cron.stop_signal` (default `SIGTERM`, issue #325) to the child first | same: `process_control_timeout` must be **≥** `cron.timeout` when `cron.timeout > 0`, or the master wins | `cron.timeout` when set |
| `status` | none (no PHP work to finish) | `process_control_timeout` only | none |

`process_control_timeout` is a **`[global]`** directive. It applies to every
pool in the file. `supervisor.stop_timeout`, `cron.timeout`, and
`request_terminate_timeout` are **per-pool**.

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
