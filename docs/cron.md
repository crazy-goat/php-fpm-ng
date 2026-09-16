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
- **`cron.script`** (required) — the PHP script to run. A path on disk, or a
  `fpmng-dist://` path naming a script embedded in the binary
  ([`docs/payload.md`](payload.md)); an embedded name that is not there is
  rejected at startup.
- **`cron.timeout`** (optional, seconds, default: no limit) — kills the
  script's process if it runs longer than this.
- **`cron.timezone`** (optional, default: unset = UTC) — an IANA zone name,
  e.g. `Europe/Warsaw`. See "Time zone and DST" below.
- **`cron.log`** (optional, default: unset = no extra log) — a file path. One
  line is appended per completed run: start time (UTC, ISO 8601), exit code,
  duration. See "Run history" below.
- **`cron.jitter`** (optional, seconds, default: unset = `0` = no jitter) —
  the maximum delay added *after* the scheduled time is already due. See
  "Jitter: avoiding a thundering herd" below.
- **`cron.jitter_mode`** (optional, `random` or `stable`, default: `random`) —
  only meaningful when `cron.jitter` is set. `random` picks a new delay
  within `[0, cron.jitter]` on every run; `stable` derives one fixed delay
  from the pool's name, so the same pool always lands at the same offset.
- **`cron.stop_signal`** (optional, default: `TERM`) — which signal the
  master sends to an actively-running cron child on shutdown or reload,
  instead of the hardcoded `SIGTERM` every other non-request-serving pool
  still gets. One of `TERM`, `QUIT`, `USR1` or `USR2`. See "Shutdown and
  `docker stop`" below.
- **`cron.expect_within`** (optional, seconds, default: unset = disabled) —
  surfaces an overrunning or stuck run as stale. See "`cron.expect_within`
  (issue #327)" below.

## Jitter: avoiding a thundering herd

Every pool sharing a schedule (e.g. `*/5 * * * *`) is due at the exact same
wall-clock second by default — with several such pools that is a small spike
of simultaneous script starts every interval. `cron.jitter` spreads that out
by adding a delay, modeled on systemd's `RandomizedDelaySec=`/
`FixedRandomDelay=`:

- **`cron.jitter_mode = random`** (default once `cron.jitter` is set) — a
  fresh random delay in `[0, cron.jitter]` seconds is picked each time the
  pool's process starts. Two consecutive runs of the same pool can land at
  different offsets from the scheduled time.
- **`cron.jitter_mode = stable`** — the delay is derived only from the pool's
  `name`, so it is the same on every run of that pool (no jitter between
  consecutive runs of the *same* pool), while pools with different names
  still spread apart from each other.

Jitter never changes how the *next due minute itself* is computed: that
decision is still made exactly as described above, with no catch-up of
missed runs (see below) — jitter only adds a delay *after* that decision was
made. Leaving `cron.jitter` unset (the default) is exactly today's
exact-time-fire behavior.

This does **not** mean a large `cron.jitter` is free: the delayed run still
has to finish and exit before its process is respawned for the *next*
schedule check, and that next check computes its own due minute from
whatever the clock reads at that point — same as always, no memory of what
was "supposed" to happen earlier. So a `cron.jitter` comparable to or larger
than the interval between scheduled ticks (e.g. `cron.jitter = 120` on
`* * * * *`) can make intervening ticks disappear in practice: the pool
simply runs less often than the un-jittered schedule implies, exactly as if
the missing ticks had never been due. **Keep `cron.jitter` well below the
schedule's own interval** — this is not a corner case to reason carefully
about, it is the entire point of the "well below" guidance.

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

## `cron.expect_within` (issue #327)

This does **not** change the "no catch-up" decision above — it makes an
overrunning or stuck run *visible*, nothing more. Once at least one run has
started, fpm-ng knows what the schedule's next occurrence after that run
should have been (`fpm_cron_schedule_next()`, the same computation
`next_run` on the status page uses). `cron.expect_within = <seconds>` says
how far past that next occurrence is still "normal" — e.g. the previous run
overran a little, or the machine was briefly busy. Once that grace period is
also gone (the schedule's next run was due more than `cron.expect_within`
seconds ago and the pool has not started a fresh run since), the pool is
"stale":

- One line is logged at `WARNING` level, once per stale episode — it re-arms
  the next time the pool goes stale again, the same convention the
  fast-restart warning ([`supervisor.md`](supervisor.md#the-fast-restart-warning))
  uses, so a schedule that recovers and later slips again is reported each
  time rather than only the first.
- The status page (`pm.status_path`) gains a `stale` field (`true`/`false`,
  plus `stale_since` — the schedule's due time — while `stale` is `true`), and
  the metrics page gains `fpmng_pool_stale` (`1`/`0`, plus
  `fpmng_pool_stale_since_seconds` while it is `1`). Both are present as soon
  as `cron.expect_within` is set, even before the pool's first run — a pool
  that never asked for staleness detection reports neither field, rather than
  an always-`false` one that looks like a check that ran and passed; a pool
  that did ask always reports at least `stale`, never only sometimes.

**Staleness is computed when the status/metrics page is rendered, not on a
timer of its own.** There is no independent master-side clock ticking away
checking every cron pool's schedule; the check above (and the `WARNING` log
line) only runs as part of answering a request to `pm.status_path` or the
metrics endpoint. A pool with `cron.expect_within` set but nothing ever
scraping its operator endpoint can sit stale, undetected, indefinitely — the
directive makes staleness *visible to whoever looks*, it does not make fpm-ng
notice on its own. In practice this means: point something (even an
occasional cron-job-watching-the-cron-job, or your existing metrics scraper)
at the pool's status or metrics page if you want the `WARNING` line and the
`stale` field to actually appear when they should.

What "stale" does **not** do: it never starts a run, never touches
`cron_term_requested`, the pool's own sleep loop, or `fpm_children.c`'s
respawn logic. A stale pool keeps running (or waiting for its current run to
finish) exactly as it would with `cron.expect_within` unset — the only
difference is that the condition is now visible to a human reading the log
or a status/metrics scrape, instead of silently indistinguishable from "the
schedule just hasn't come around yet." Pick a value comfortably larger than
the job's normal run time and the schedule's own period combined — too small
and every ordinary run overlapping the next tick logs a spurious warning; too
large and a genuinely stuck job goes unnoticed for that much longer.

## Run history

The pool's status page (`pm.status_path`) shows the last run only: whether it
is running right now, when it last started, the exit code of the last completed run, and how
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

## Script output: `STDOUT`, `STDERR` and `STDIN`

The script runs with the three standard stream constants defined, exactly as
under the `php` CLI binary — `fwrite(STDERR, "...")` works and is not a fatal
error. Where they lead is a property of FPM, not of the script:

- **`STDOUT` and `STDERR`** go to the master only when the pool sets
  `catch_workers_output = yes`, and each line then appears in `error_log` as
  `WARNING: [pool <name>] child <pid> said into stdout: "..."`. Without that
  directive both descriptors are `/dev/null` and everything written to them is
  discarded — that is upstream FPM's default for a worker's stdout and this
  pool type does not change it.
- **`STDIN`** is `/dev/null`: a valid handle whose first read returns an empty
  string and sets EOF, rather than blocking or raising an error. A script that
  needs real input has to get it from a file, the environment, or the network
  — there is no way to feed a cron pool's script on stdin.

For output you actually want kept, use **`error_log()`**: in a `cron` or
`supervisor` pool it reaches FPM's `error_log` with the pool name in the line
and needs no `catch_workers_output` (issue #124). `catch_workers_output` is
worth its cost mainly when the script's output is not yours to change — a
third-party command-line tool run as a cron job, for example. Note the price:
it carries every line the child writes, and for a `supervisor` pool restarting
a short script in a loop that is a large amount of log (measured: 52 MB in
15 s, see the comment in `sapi/fpmng/tests/fpmng-supervisor-restart.phpt`).

For exactly that case — output that is not yours to change, so `error_log()`
is not an option, but you still do not want to pay `catch_workers_output`'s
per-line master round trip — set **`cron.output_log`** to a file path.
`STDOUT` and `STDERR` are then redirected straight to that file (append,
`O_CREAT`, no truncation) once, before the script runs, bypassing
`catch_workers_output`'s pipe and the master's reader thread entirely,
whether or not `catch_workers_output` is also set on the same pool. It is a
plain append-only file with no size limit or rotation built in, the same
expectation as `cron.log` above — the operator's own job to rotate. It is
also a different file from `cron.log`: `cron.log` is run history (start time,
exit code, duration) written by FPM itself; `cron.output_log` is whatever the
script itself writes to `STDOUT`/`STDERR` (issue #328).

## Shutdown and `docker stop`

When the master receives `SIGTERM` (e.g. `docker stop`), a cron child that is
**sleeping** before the next run exits immediately and skips that run. A child
that is **already running a script** is asked to stop with `cron.stop_signal`
(default `TERM`, unchanged from before this directive existed) through the
master's `process_control_timeout` escalation unless you raise that global
value; when you set `cron.timeout`, it must be **≤** `process_control_timeout`
or the master kills the run before `cron.timeout` can act. See
[`docs/shutdown-timeouts.md`](shutdown-timeouts.md) for the full table,
defaults, and startup warnings.

### `cron.stop_signal`

Every other non-request-serving pool type is asked to stop with a hardcoded
`SIGTERM` by the master's own shutdown/reload escalation
(`fpm_pctl_kill_all()` in `fpm_process_ctl.c`, unchanged for those types).
`cron.stop_signal` gives a cron pool a way to ask for something softer
instead — `QUIT`, `USR1` or `USR2` — so a script that traps that signal
(`pcntl_signal()`) can flush a buffer, checkpoint progress, or otherwise wind
down cleanly before `cron.timeout` (or, in its absence,
`process_control_timeout`) forces a `SIGKILL`.

Unlike `supervisor.stop_signal` (issue #324), which the master cannot honor
directly (a self-triggered recycle sends the configured signal to itself, but
`fpm_pctl_kill_all()` still hardcodes `SIGTERM` for that type), a cron pool's
`stop_signal` **is** what the master sends on `docker stop` or a config
reload that removes or changes this pool — the substitution happens in
`fpm_pctl_kill_all()` itself, driven by `fpm_pool_type_s.stop_signal`
(`fpm_pool_cron_stop_signal()` in `fpm_pool_cron.c`). This only ever replaces
a plan that was already "send `SIGTERM`" for a non-request-serving pool: the
first `SIGQUIT` a request-serving pool gets, and the final `SIGKILL`
escalation for every pool type, are both untouched — `cron.timeout`'s
watchdog remains the hard fallback regardless of this directive.

`fpm_pool_cron_child_main()` always keeps its own `SIGTERM` handler
installed regardless of `cron.stop_signal`, for the same reason
`supervisor.stop_signal` does (`docs/supervisor.md`): an operator sending
`SIGTERM` directly to the child (unusual outside manual debugging) must
still get a clean stop, not the process default's immediate kill. Setting
`cron.stop_signal` to something other than `TERM` installs a SECOND handler
for that signal, in addition to `SIGTERM`'s — it does not replace it.

Leaving `cron.stop_signal` unset is exactly today's behavior: `SIGTERM`.
