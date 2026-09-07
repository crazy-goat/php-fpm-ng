# 033 — Revisit three deliberate limits in `pool.type = cron`

**Priority:** medium. None of these are bugs — the header comment justifies
each — but the product's stated target (small projects on one VPS) is
exactly the audience most likely to be surprised by them, so they deserve a
written, current decision rather than standing on a comment written at
design time.
**Status:** open.

## Context

`fpm_pool_cron.c:1-40` is a design-rationale comment, not a changelog. It is
explicit that these are choices: "to bedzie zmiana projektowa, nie poprawka"
(line 33, of the missed-run limit) — this task is exactly that kind of
revisit, done on purpose, not a bug report.

This task does **not** propose fixing these as defects. It asks, for each,
one question: given the target audience (small projects, one VPS, no
ops team watching a dashboard), is the current behaviour still the right
call, or does it need to change — and either way, is it documented where an
operator will actually read it before being surprised by it?

## (a) UTC only, no local time, no DST — `fpm_pool_cron.c:26-28`

> "CZAS: liczymy wylacznie w UTC (fpm_cron_schedule_next() uzywa
> gmtime_r()). Czas lokalny + zmiana czasu (DST) daje przebieg podwojny albo
> zaden przy kazdym przejsciu — nie robimy tego."

Confirmed: `fpm_cron_schedule_next()` is called with `gmtime_r()`
throughout `fpm_pool_cron.c` (e.g. the status computation at line ~353 calls
it against `time(NULL)`, interpreted as UTC).

**What this means for the stated audience:** someone who wants "a report at
3am my time" has to compute their own UTC offset and hand-translate it into
`cron.schedule`, and re-translate it twice a year if their timezone observes
DST. The reasoning is written down, but only in `docs/NOTES.md:1435-1441`
("Czas: wyłącznie UTC" — nearly the same wording as the source comment) and
again, earlier and more tersely, in `docs/NOTES.md:127-128` (point 2 of "Cztery
rzeczy do rozstrzygnięcia"). `docs/NOTES.md` opens by describing itself as
"pamięć projektu" — a development journal (decisions, measurements, open
questions) — not operator documentation. There is no README or user guide
in this repository that documents `cron.schedule` at all: `README.md` (84
lines) never mentions it. **So the decision itself looks sound, but it currently
lives only where a developer reading source or dev notes would find it, not
where an operator writing a `cron.schedule` line would.**

## (b) No catch-up for missed runs — `fpm_pool_cron.c:30-34`

> "NIE NADRABIAMY zgubionych przebiegow. fpm_cron_schedule_next() zawsze
> liczy 'co jest najblizej w przyszlosci od teraz', nigdy 'co przegapilem
> odkad ostatnio dzialalem' — jesli master byl wylaczony godzine, nastepny
> przebieg to najblizszy przyszly termin, nie dwanascie zaleglych."

**What this means for the stated audience:** a single VPS is exactly the
environment where the whole machine goes down for a kernel update, a reboot,
a resource limit — and a daily backup or report scheduled during that window
does not run at all, silently, with nothing to indicate it was skipped
rather than simply not due yet. Contrast with `pm = static` /
`fpm_children.c`'s unconditional, immediate respawn, which the header itself
leans on as a simplifying assumption elsewhere in the file — that assumption
covers "the cron pool's *process* comes back," not "the *schedule* it
missed gets made up."

This gap was anticipated, not discovered: `docs/NOTES.md:1430-1432` already
says, in the same words as the source header, "ktoś kiedyś będzie chciał
dodać nadrabianie zaległych przebiegów — to ma być świadoma zmiana
projektowa ..., nie poprawka," and `docs/NOTES.md:129-130` (point 3 of the
same four-point list referenced above) flags it again from the original
design pass. The decision was made deliberately and recorded twice in dev
notes; it has never been carried into anything an operator reads.

## (c) No run history beyond the last one — `fpm_pool_cron.c:95-115, 337-365`

The shared-memory struct (`fpm_cron_shared_s`, `fpm_pool_cron.c:103-110`)
keeps exactly three things: `running`, `last_run` (epoch of the last start),
`last_exit_code` (+ `has_last_exit_code`), and `consecutive_failures`. The
comment at line 100-102 is explicit that this is deliberately minimal and
that cron never reads any of it back to make a decision — it exists "WYLACZNIE
do odczytu przez pool.type = status."

`fpm_pool_cron_status()` (`fpm_pool_cron.c:337-365`) additionally computes
`next_run` **live**, from the current clock and the parsed schedule, every
time it's asked (not stored) — so `pool.type = status` can show: is it
running right now, when did it last start, did the last run succeed, how
many failures in a row, and when is the next run due.

**What this does and does not answer for "cron silently stopped working":**

- It answers "did the last run fail" (via `last_exit_code` /
  `consecutive_failures`), and that failure is logged at `ZLOG_WARNING`
  (`fpm_pool_cron.c`, non-zero exit code branch), which is loud enough for a
  single-instance deployment per its own comment.
- It does **not** keep a count or a log of how many runs have happened in
  total, nor any run older than the most recent one. An operator polling
  `/status` sees one data point, not a trend.
- It does **not** flag "overdue" itself. `next_run` is always the *next
  future* occurrence (per point (b), `fpm_cron_schedule_next()` never looks
  backward), so if the cron pool's process were somehow never actually
  invoked (not because of (b)'s master-downtime case, which is a schedule
  gap, but a hypothetical bug that stalls the respawn) `next_run` would
  still show a plausible-looking near-future time — nothing distinguishes
  "waiting for a legitimately near time" from "should have already run and
  didn't." This is architectural: `next_run` is computed fresh from the
  schedule, deliberately with no memory of what actually happened, so it is
  structurally unable to detect that class of failure. Confirmed by reading
  the function; not measured against a live stall, so treat the "should have
  run and didn't" scenario as a plausible failure mode, not an observed one.

## Problem

For each of the three points, produce a written decision: change the
behaviour, or record it as a permanent limitation in user-facing
documentation (not just the source comment, which only a code reader sees).

## Acceptance criteria

1. **(a) UTC/DST:** either `cron.schedule` gains a way to express "local
   time" (a design change, per the header's own words, not a small patch),
   or `docs/` gains an explicit, example-driven note: "compute your UTC
   schedule from your local time; re-derive it if your timezone observes
   DST" — placed next to the `cron.schedule` directive documentation, not
   only in a source comment nobody configuring a pool will ever open.
2. **(b) Missed runs:** either a catch-up mode is designed (explicitly framed
   as the design change the header anticipates, with a decision on how far
   back to look and how to avoid a thundering-herd of catch-up runs after a
   long outage), or `docs/` states plainly, next to `cron.schedule`, that a
   missed window is skipped, not deferred — so an operator relying on cron
   for anything time-sensitive (backups, expiries) makes an informed choice
   about whether that's acceptable for their use case.
3. **(c) Run history:** a decision on whether `pool.type = status` needs
   more than "last run" — e.g. a running total of executions, or an explicit
   "overdue" computation comparing the last known `next_run` against the
   current clock at read time. If the decision is "last-run-only is enough,"
   write down why, referencing the single-instance-VPS assumption the rest
   of the file already leans on.
4. Whichever way each point is decided, the decision is written where an
   **operator**, not a code reader, will find it — `docs/`, not only a
   source comment.

## Explicitly out of scope

- Changing `fpm_children.c` or the master's process-respawn machinery. All
  three points are about the schedule/status layer, not about whether the
  process comes back (it already does, unconditionally, per the header).
- Any implementation before the decision is written down.

## Notes

- All three points are explicitly called out as intentional simplifications
  in the same header comment, alongside two design pillars this task does
  not touch: no timers in the master (lines 4-13) and no shared control
  state driving cron's own behaviour (lines 16-24, `consecutive_failures`
  exists "na nic nie wplywa" per the struct's own comment). Neither of those
  two is in scope here; this task is only about (a), (b) and (c) as framed
  above.

## Outcome

- **(a) UTC/DST:** changed behaviour. Added optional `cron.timezone` (IANA
  zone name; unset = UTC, unchanged default). `fpm_cron_schedule_next()`
  takes a `tz` parameter and uses `localtime_r()` with the `TZ` environment
  variable set/restored around the search when given, `gmtime_r()`
  otherwise (`fpm_cron_schedule.c`, `fpm_pool_cron.c`). The zone name is
  validated at pool startup against the system zoneinfo database, rejected
  like a bad `cron.schedule` if unknown. DST transition edge cases (a
  schedule skipped once on "spring forward", fired twice on "fall back")
  are documented as an accepted quirk, not fixed further — same as
  `crontab(5)` in local time.
- **(b) Missed runs:** documented as a permanent limitation, not changed.
  No catch-up mode was designed.
- **(c) Run history:** added optional `cron.log` (a file path; unset = no
  extra log). One line appended per completed run (start time, exit code,
  duration), no rotation. `pool.type = status` itself is unchanged — still
  last-run-only, per the single-instance-VPS assumption the rest of the
  file leans on; a running total or an "overdue" computation was not built,
  since `cron.log` already answers "did cron actually run and how often"
  for an operator willing to read a file.
- All three decisions and the two new directives are documented for
  operators in the new `docs/cron.md` (linked from `README.md`), not only
  in source comments.
