# 034 — Define "scheduler" against cron and supervisor, or drop the word

**Priority:** low until decided; nothing depends on this, and picking it up
mainly prevents someone building against a word that turns out to mean
nothing new.
**Status:** open. Decision task — no implementation should start against
this file.

## Context

The project owner has named "scheduler" as a direction alongside cron,
proxy, packaging and the rest of the main line (see `tasks/README.md`'s
description of the main line: "the HTTP gateway, cron, the scheduler,
proxying, packaging, CI and the tests"). No requirement beyond the word has
been given, and no code or directive named `scheduler` exists — checked,
`grep` for `scheduler` across `sapi/fpmng/fpm/*.c` and `*.h` finds nothing.

Two pool types already exist that plausibly cover what "scheduler" might
mean:

- **`pool.type = cron`** (`fpm_pool_cron.c`) — time-based, one-shot runs.
  Schedule expressed as a crontab-style expression (`cron.schedule`),
  parsed by `fpm_cron_schedule.c`. Design is deliberately minimal: no
  timers in the master, no shared control state, no catch-up of missed
  runs, UTC only (see task 033 for a full revisit of these). Each run is a
  single process that runs the configured script once and exits.
- **`pool.type = supervisor`** (`fpm_pool_supervisor.c`) — long-lived
  processes with restart policy. `supervisor.processes` maps to `pm =
  static` + `pm.max_children`, so spawning/respawning is FPM's existing
  child machinery (`fpm_children.c`, untouched, per
  `fpm_pool_supervisor.c:4-6`). What `fpm_children.c` cannot do —
  withholding a respawn — lives in shared memory per pool and is enforced
  by the child itself at start and between script runs
  (`fpm_pool_supervisor.c:6-9`): `supervisor.restart` (`always` / `never` /
  `on-failure`), `supervisor.restart_delay`,
  `supervisor.restart_delay_max`, `supervisor.restart_max`,
  `supervisor.fatal`.

Both share infrastructure: the pidfd watchdog for a stop timeout
(`fpm_pool_watchdog.c`, used by both `cron.timeout` and
`supervisor.stop_timeout`) and script execution outside the FastCGI request
path (`fpm_pool_script.c`).

## What "scheduler" could mean, if it's a third thing

Candidates, none requested, listed so whoever decides this has the menu:

1. **A job queue.** Application code enqueues units of work (e.g. "send
   this email," "process this upload") for a pool of workers to drain,
   rather than a fixed time-based trigger. Neither `cron` (time-triggered)
   nor `supervisor` (a single long-lived process, not a worker pool
   draining a queue) covers this today.
2. **Dependent/chained jobs.** "Run B after A succeeds." Cron's model is
   independent, non-overlapping runs of one script on one schedule with no
   shared state between runs (task 033, point (c)) — there is nowhere for
   "A succeeded" to live today, by design.
3. **Event-triggered runs.** Something other than wall-clock time kicks off
   a run (a file appearing, a signal, an HTTP callback). Structurally
   different from `cron.schedule`, which is purely time-based.
4. **Delayed / one-off future jobs.** "Run this once, at this specific
   future timestamp," as opposed to a recurring crontab pattern. Closer to
   cron than the other three, but not quite what `cron.schedule` expresses
   today (a recurring pattern, not a single instant).

## Or, "scheduler" is just cron with a better name

Everything named above beyond a recurring time-based trigger is a
substantial addition (a queue, dependency tracking, event wiring) that would
need its own design, its own shared-memory shape, and likely its own
`pool.type`, not a rename. If none of these four has actually been asked
for, the honest reading is that "scheduler" was used loosely to mean "the
thing that runs jobs on a timer" — i.e. cron — and the word should be
retired rather than implying a fourth, undesigned pool type sits in the
gap between `cron` and `supervisor`.

## Problem

Decide, in writing: is "scheduler" one of the four candidates above (or
something else), a different name for `cron`, or nothing that needs
building right now. **This requirement has not been given by the project
owner** — the word appears once, in a task-tracking document, with no
elaboration.

## Acceptance criteria

1. A written decision, one of:
   - "scheduler" names a specific capability from the list above (or one not
     listed), with a description of what it needs that `cron` and
     `supervisor` do not already provide, and a rough size estimate — this
     becomes a new task, not this one.
   - "scheduler" is `cron`; the word is retired from `tasks/README.md` and
     anywhere else it appears as if it named separate work.
2. Either way, `tasks/README.md`'s main-line description is updated to match
   the decision, so the next person reading it does not go looking for a
   third pool type that was never meant to exist.
3. If the decision surfaces new requirements for `cron` itself (e.g. a job
   queue turns out to be what's wanted), check first whether that overlaps
   task 033's points before filing new work — a job queue with dependency
   tracking is a bigger change than anything task 033 revisits, but a
   "delayed one-off job" might just be a `cron.schedule` expressiveness gap
   that belongs there instead.

## Explicitly out of scope

- Any implementation. This is a decision task.
- Task 033's three points (UTC/DST, missed runs, run history) — those are a
  revisit of existing `cron` behaviour, not a definition of "scheduler."

## Notes

- A legitimate, acceptable outcome of this task is "this is cron, we retire
  the word" — that is not a lesser outcome than defining a new capability,
  and it's the one this task's author considers most likely given how the
  word was introduced (a one-line mention, no elaboration, alongside four
  other main-line nouns that all already have code or an open task).

## Outcome

Decided: "scheduler" named no capability beyond `cron`. None of the four
candidates (job queue, dependent jobs, event-triggered runs, delayed one-off
jobs) had actually been asked for. Retired the word from
`tasks/README.md`'s main-line description, with a note pointing at this
file for why. No code changes — this was a naming decision only.
