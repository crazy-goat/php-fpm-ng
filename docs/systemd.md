# systemd notification (`Type=notify-reload`)

The master of php-fpm-ng sends messages to systemd. The `.deb` unit,
`packaging/deb/php-fpm-ng.service`, uses `Type=notify-reload`. This type needs
systemd 253 or later. The packaged target, ubuntu:26.04, has systemd 259.
The master sends the messages itself. It does not need libsystemd.

With this unit:

- `systemctl start` waits for the first `READY=1`.
- `systemctl reload` waits for the `READY=1` of the new master.

## The messages

Only the master sends messages. The children do not send them, because the unit
sets `NotifyAccess=main`. Each message is one datagram to the socket that
`NOTIFY_SOCKET` names.

| Message | Sent when | Meaning |
|---|---|---|
| `READY=1` | Every pool has bound its listening socket and the first children are started. | The listening sockets are bound. This does not mean that a pool or a gateway answers requests. |
| `RELOADING=1` with `MONOTONIC_USEC`, then `READY=1` | A reload that the configuration check refuses. | The master keeps its current generation. The `READY=1` ends the reload job of systemd. |
| `RELOADING=1` with `MONOTONIC_USEC` | A reload (`SIGUSR2`) passes the configuration check. | A new generation of the master starts. Its `READY=1` follows when its pools listen again. |
| `STOPPING=1` | The stop starts: `SIGQUIT`, `SIGTERM` or `SIGINT` reaches the master. | The master starts to stop. |
| `WATCHDOG=1` | Every half of `WATCHDOG_USEC`, when systemd sets it. | The event loop of the master runs. The packaged unit does not set it. |

## Reload

A reload from `systemctl reload` has these steps:

1. systemd runs the `ExecReload=` line. It runs the configuration check
   (`-t`). If the check fails, the reload job fails. systemd sends no signal.
2. systemd sends `SIGUSR2` (`ReloadSignal=`). The master checks the
   configuration again. If the check passes, the master sends `RELOADING=1`
   and starts the new generation.
3. The new generation sends `READY=1` when its pools listen again. systemd
   ends the reload job then, and `systemctl reload` returns.

A reload runs the master again with the same process ID. The new master keeps
`NOTIFY_SOCKET`, so it can send its messages.

A reload that the configuration check refuses inside the master sends
`RELOADING=1` with `MONOTONIC_USEC`, then `READY=1`. The master keeps the
current generation, so `READY=1` is true. A bare `READY=1` does not end the
reload job. Without the pair, systemd waits until `TimeoutStartSec=` expires.
The first step rules out most broken configurations. The second step catches a
change made after the first step ran.

A `SIGUSR2` that does not come from `systemctl reload` runs no check in the
unit. The master still checks the configuration itself. A refused reload then
sends the same pair.

Measured on systemd 259 with the packaged unit (paths changed), on a test box
shared with other runs:

- Start, valid configuration, 3 runs: `systemctl start` returns 57 ms to 95 ms
  after the start.
- Stop, 3 runs: `systemctl stop` returns 45 ms to 80 ms after the stop.
- Valid reload, 3 runs: `systemctl reload` returns with exit status 0 after
  116 ms to 163 ms. The new generation listens.
- Refused by the `-t` line of the unit, 1 run: `systemctl reload` fails with
  exit status 1 after 60 ms. The unit stays active.
- Refused inside the master, 1 run: `systemctl reload` returns with exit status 0
  after 101 ms. The unit stays active and the pools keep listening.

The reason for a refused reload is in the error log. The first `ExecReload=`
line of the unit also writes the reason to the journal (see `docs/reload.md`).

## The watchdog

The watchdog is off by default. The packaged unit has no `WatchdogSec=` line.

Do not run step 3 under load. The restart stops all pools.

To use the watchdog:

1. Run `systemctl edit php-fpm-ng`.
2. Add `WatchdogSec=30` in the drop-in, then save the file.
3. Run `systemctl restart php-fpm-ng`.

systemd sets `WATCHDOG_USEC` from `WatchdogSec=`. The master sends `WATCHDOG=1`
every half of that time. The master ignores `WATCHDOG_USEC` when `NOTIFY_SOCKET`
is not set, and when `WATCHDOG_PID` names another process.

The pings come from the event loop of the master. If the loop stops, the pings
stop. systemd then kills the master and marks the service as failed. The
`Restart=` setting decides whether systemd starts the service again. The
packaged unit does not set `Restart=`.

## Failed sends

The master does not stop when a send fails. One example is a `NOTIFY_SOCKET`
that names a path with no socket. The master uses a non-blocking socket, so a
send never waits.

The master writes one WARNING to the error log for each master process:
`sd_notify: cannot send to NOTIFY_SOCKET`. Later failures in the same process
are not logged. A reload starts the master again with new state, so its first
failure is logged too.

## Without systemd

If `NOTIFY_SOCKET` is unset or empty, the master sends nothing. It writes
nothing about the missing socket. It also ignores `WATCHDOG_USEC`, so a value
that is not a number is not reported either. The master runs the same way as
under systemd.

## What the master does not send

The master does not send `STATUS=`, `FDSTORE=` or `MAINPID=`. A `READY=1`
does not prove that a gateway accepts requests. To check that a pool answers,
use the operator endpoint (see `docs/operator-endpoint.md`).
