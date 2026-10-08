# systemd notification (`Type=notify`)

The master of php-fpm-ng sends messages to systemd. The `.deb` unit,
`packaging/deb/php-fpm-ng.service`, uses `Type=notify`. With this type, systemd
waits for the first `READY=1` before it marks the service as started. The master
sends the messages itself. It does not need libsystemd.

## The messages

Only the master sends messages. The children do not send them, because the unit
sets `NotifyAccess=main`. Each message is one datagram to the socket that
`NOTIFY_SOCKET` names.

| Message | Sent when | Meaning |
|---|---|---|
| `READY=1` | Every pool has bound its listening socket and the first children are started. | The listening sockets are bound. This does not mean that a pool or a gateway answers requests. |
| `RELOADING=1` with `MONOTONIC_USEC` | A reload (`SIGUSR2`) passes the configuration check. | A new generation of the master starts. Its `READY=1` follows when its pools listen again. |
| `STOPPING=1` | The stop starts: `SIGQUIT`, `SIGTERM` or `SIGINT` reaches the master. | The master starts to stop. |
| `WATCHDOG=1` | Every half of `WATCHDOG_USEC`, when systemd sets it. | The event loop of the master runs. The packaged unit does not set it. |

## Reload

A reload that passes the configuration check sends `RELOADING=1`, then
`READY=1`. A reload runs the master again with the same process ID. The new
master keeps `NOTIFY_SOCKET`, so it can send its messages.

`systemctl reload` does not wait for that `READY=1`. The packaged unit has
`Type=notify`, so systemd ends the reload when the `ExecReload=` commands exit.
The second command, `kill -USR2`, exits when the signal is sent. The new master
starts after that and sends `READY=1` when its pools listen. Measured on systemd
259 with the packaged unit (paths changed): `systemctl reload` returned 80 ms to
121 ms after it started. The new master logged its first line about 50 ms after
`Reloaded` (3 runs). A script that needs the new master must wait for it.

A reload that the configuration check refuses sends nothing. The reason is in
the error log. The first `ExecReload=` line of the unit also writes the reason
to the journal (see `docs/reload.md`).

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
