# `pool.type = cron` — operator reference

This is the operator-facing reference for `pool.type = cron`: what each
directive does, and the two behaviours that are permanent, deliberate
limitations rather than bugs (task 033). For the internal design rationale
(why no timers in the master, why no shared control state), see the header
comment in `sapi/fpmng/fpm/fpm_pool_cron.c` and `docs/NOTES.md`.

A cron pool runs one script once per matching schedule tick, as a single
process (`pm = static`, `pm.max_children = 1`, not configurable). There is no
overlap: the next run cannot start until the previous one has exited.

## Directives

- **`cron.schedule`** (required) — a 5-field crontab(5) expression (minute
  hour day-of-month month day-of-week) or one of `@hourly`, `@daily`,
  `@weekly`, `@monthly`, `@yearly`. Invalid syntax is rejected at startup with
  a specific error, not accepted and silently never run.
- **`cron.script`** (required) — the PHP script to run.
- **`cron.timeout`** (optional, seconds, default: no limit) — kills the
  script's process if it runs longer than this.
- **`cron.timezone`** (optional, default: unset = UTC) — an IANA zone name,
  e.g. `Europe/Warsaw`. See "Time zone and DST" below.
- **`cron.log`** (optional, default: unset = no extra log) — a file path. One
  line is appended per completed run: start time (UTC, ISO 8601), exit code,
  duration. See "Run history" below.

## Time zone and DST

By default `cron.schedule` is evaluated in **UTC**. If your schedule means
"3am for me", compute the UTC equivalent of your local time and put that in
`cron.schedule` — and recompute it if your time zone observes DST, since UTC
does not shift with it.

Setting `cron.timezone` avoids that manual translation: the schedule's
fields are matched against that zone's local time instead, using the
system's own time zone database (the zone name is validated at startup the
same way `cron.schedule` is — an unknown name is rejected, not silently
treated as UTC).

This has one accepted consequence, not a bug: around a DST transition, a
schedule can either be skipped once (when the transition removes an hour —
"spring forward" — and the scheduled wall-clock time never occurs that day)
or run twice (when the transition repeats an hour — "fall back" — and the
scheduled wall-clock time occurs twice that day). This is exactly what
`crontab(5)` does when run in local time; it is a once-a-year quirk that
this project accepts rather than tries to paper over. If a schedule must
never run twice or never be skipped, keep `cron.timezone` unset and use UTC.

## Missed runs are skipped, not deferred

If the whole machine is down when a scheduled time passes — a reboot, a
resource limit, a maintenance window — that run does **not** happen later.
When the cron pool's process next starts, it computes the next run strictly
after the current time; it never looks backward for what it missed. A daily
job scheduled during an hour-long outage simply does not run that day.

There is currently no catch-up mode. If a job is time-sensitive (a backup, an
expiring token, anything where "it ran a day late" is materially different
from "it ran on time"), account for that when deciding whether `pool.type =
cron` is the right tool, or build a compensating check into the script
itself (e.g. verify the last successful backup's age and warn if it is
stale).

## Run history

`pool.type = status` shows the last run only: whether it is running right
now, when it last started, the exit code of the last completed run, and how
many times in a row it has failed. It does not keep a count of total runs,
and it cannot tell "waiting for a legitimately near scheduled time" apart
from "should have already run and did not" — `next_run` is always computed
fresh from the schedule and the current clock, with no memory of what
actually happened.

Set `cron.log` to a file path to get a history beyond the last run: one
appended line per completed run (start time, exit code, duration), with no
size limit or rotation built in — that is the operator's responsibility, the
same as any other log file this project writes. Failures are also logged at
warning level regardless of `cron.log`, which is loud enough to notice on a
single-instance deployment.
