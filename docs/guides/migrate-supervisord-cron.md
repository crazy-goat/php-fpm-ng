# Migrating from supervisord + cron

supervisord keeps long-running consumers alive, cron starts periodic jobs. In
php-fpm-ng each is a pool in the same file as the web pools: `pool.type =
supervisor` and `pool.type = cron`. Both run a **PHP script** inside a worker that
has booted the interpreter once, with the same `user`, `php_admin_value[...]` and
`env[...]` vocabulary as any FPM pool.

The constraint to know first: **the unit of work is a PHP script file**, not a
shell command. `command=php /srv/app/bin/console messenger:consume async` and
`php artisan schedule:run` become a small PHP file that calls into the
application. Anything that is not PHP (a shell pipeline, `pg_dump`, a Node
script) stays in a system cron or supervisord, or is started from PHP with
`proc_open()`.

## Before

`/etc/supervisor/conf.d/worker.conf`:

```ini
[program:worker]
command=php /srv/app/bin/worker.php
user=www-data
numprocs=2
process_name=%(program_name)s_%(process_num)02d
autorestart=true
startretries=10
stopsignal=TERM
stopwaitsecs=30
environment=APP_ENV="prod"
```

`/etc/cron.d/app`:

```
MAILTO=""
CRON_TZ=Europe/Warsaw
*/5 * * * * www-data php /srv/app/bin/report.php >> /var/log/app/report.log 2>&1
0 3 * * *   www-data php /srv/app/bin/cleanup.php
```

## After

```ini verify
[global]
error_log = /var/log/php-fpm-ng/error.log
daemonize = no
; at least the largest supervisor.stop_timeout / cron.timeout, or the master
; kills the child before that grace runs out (docs/shutdown-timeouts.md)
process_control_timeout = 35s

[worker]
pool.type = supervisor
user = www-data
group = www-data
supervisor.script = /srv/app/bin/worker.php   ; command=php ...
supervisor.processes = 2                      ; numprocs
supervisor.restart = always                   ; autorestart=true
supervisor.restart_delay = 1
supervisor.restart_max = 10                   ; startretries
supervisor.stop_signal = TERM                 ; stopsignal
supervisor.stop_timeout = 30s                 ; stopwaitsecs
supervisor.output_log = /var/log/php-fpm-ng/worker.out ; stdout_logfile
env[APP_ENV] = prod                           ; environment
operator.status = on                          ; supervisorctl status: /status/worker

[report]
pool.type = cron
user = www-data
group = www-data
cron.schedule = */5 * * * *
cron.timezone = Europe/Warsaw                 ; CRON_TZ
cron.script = /srv/app/bin/report.php
cron.output_log = /var/log/php-fpm-ng/report.log ; >> report.log 2>&1
cron.log = /var/log/php-fpm-ng/report.history   ; one line per run
operator.status = on                          ; /status/report

[cleanup]
pool.type = cron
user = www-data
group = www-data
cron.schedule = 0 3 * * *
cron.timezone = Europe/Warsaw
cron.script = /srv/app/bin/cleanup.php
cron.timeout = 600
```

`php-fpm-ng -t -y /etc/php-fpm-ng/php-fpm-ng.conf` checks all of it, including
that every script exists.

## supervisord map

| supervisord | php-fpm-ng | notes |
|---|---|---|
| `command=php script.php` | `supervisor.script` | a PHP file; arguments cannot be passed |
| `numprocs` | `supervisor.processes` | |
| `autorestart=true` / `unexpected` / `false` | `supervisor.restart = always` / `on-failure` / `never` | **`always` restarts a script that exits 0 at once**; the script sets the pace, so a consumer must loop or block ([`supervisor.md`](../supervisor.md)) |
| `startsecs`, delay between retries | `supervisor.restart_delay`, `supervisor.restart_delay_max`, `supervisor.restart_jitter` | the delay applies to failures |
| `startretries` | `supervisor.restart_max` | `0` (default) never gives up; `supervisor.fatal = yes` then stops the master |
| `stopsignal`, `stopwaitsecs` | `supervisor.stop_signal`, `supervisor.stop_timeout` | keep `process_control_timeout` at or above the timeout |
| `user` | `user`, `group` on the pool | |
| `environment` | `env[NAME] = value` | |
| `stdout_logfile` | `supervisor.output_log` or `error_log()` in the script | |
| memory-leak restarts (eventlistener `memmon`) | `supervisor.max_memory`, `supervisor.max_runtime` | |
| `supervisorctl status` | `operator.status = on` on the pool | JSON at `/status/<pool>` on the operator listener, `127.0.0.1:9253` by default ([`operator-endpoint.md`](../operator-endpoint.md)) |
| `supervisorctl restart` | `systemctl reload php-fpm-ng` | restarts every pool; `reload.selective = yes` restarts only the pools whose section changed ([`reload.md`](../reload.md)) |

No equivalent: process groups with ordering (`priority`), `autostart=false`
(a pool starts with the master), `supervisorctl` start/stop of one program
without a reload, and the web UI.

## cron map

| cron | php-fpm-ng | notes |
|---|---|---|
| 5-field schedule, `@hourly` ... `@yearly` | `cron.schedule` | invalid syntax refuses startup |
| the command | `cron.script` | a PHP file, no arguments, no shell |
| `CRON_TZ` | `cron.timezone` | default is UTC; DST behaviour in [`cron.md`](../cron.md#time-zone-and-dst) |
| `>> file 2>&1` | `cron.output_log` | append-only, no rotation |
| (nothing) | `cron.log` | start, exit code and duration of every run |
| `timeout 10m cmd` | `cron.timeout` | |
| `MAILTO` | none | nothing is mailed; read `cron.log` or alert on the status page (`cron.expect_within` marks a stale job) |
| `flock -n` against overlap | built in | the next run does not start until the previous one exited; **missed runs are skipped, not deferred** |
| `RANDOM_DELAY` / `sleep $((RANDOM % 60))` | `cron.jitter`, `cron.jitter_mode` | |

## Verify

`-t` first, then watch the first runs: `cron.log` gets a line per run and the
status page of a cron pool (`curl -s http://127.0.0.1:9253/status/report`) shows `state`, `runs`, `last_start`, `last_exit_code` and `next_run`. A
supervised script that returns immediately logs a fast-restart warning: fix the
loop before you rely on it.
