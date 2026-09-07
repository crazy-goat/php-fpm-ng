# 017 — `process_control_timeout` silently kills long-lived pools on shutdown

**Priority:** low-medium. Documentation and possibly a warning; no mechanism.
**Status:** done.

## Context

With FPM's default `process_control_timeout = 0`, the master escalates to
`SIGKILL` roughly one second after asking a child to stop. This is the master's
behaviour for **every** pool type, not something the new types introduced — a
plain FastCGI worker in the middle of a long request dies the same way.

It matters more for the pool types this project added. Measured: a `supervisor`
script with a 2.4 s iteration did not finish its iteration on `docker stop`. Our
own `supervisor.stop_timeout` (default 10 s) is irrelevant, because the master
kills the process before our watchdog acts.

No orphans were left; the process was reaped correctly. The only anomaly was a
missing "child exited" log line for that PID, attributed to the master's event
loop ending before it logged that particular `SIGCHLD` — cosmetic log ordering,
verified with `ps` that the process really was gone.

The conclusion recorded at the time: anyone who wants a `supervisor` or `cron`
pool to finish its work on `SIGTERM` / `docker stop` **must set
`process_control_timeout` globally**. It is an existing FPM directive, not
something to add. That conclusion currently lives only in the working journal.

## Problem

Make this reachable by a user before it bites them, and decide whether the
software should say something at startup.

## Acceptance criteria

1. The relationship between `process_control_timeout`,
   `supervisor.stop_timeout`, `cron.timeout` and `request_terminate_timeout` is
   documented in user-facing form: which one wins, in what order, and what the
   default combination actually does on `docker stop`.
2. A decision, with reasoning, on whether to emit a startup warning when a pool
   type that can hold long-lived work is configured while
   `process_control_timeout` is at its default. Both answers are defensible:
   - **warn:** the default silently truncates work the user asked to schedule
   - **do not warn:** it is upstream FPM's default and upstream's semantics, and
     a warning on a stock configuration trains people to ignore warnings
   Whichever is chosen, the reasoning is recorded.
3. If a warning is added: it names the directive, the observed consequence and
   the suggested value, fires once at startup, and never on every child spawn.

## Explicitly out of scope

- Changing the default. That is upstream FPM's, and diverging from it silently
  would be worse than the current situation.
- The missing "child exited" log line. Cosmetic, verified harmless; if it is
  worth fixing it deserves its own task.

## Outcome

Added [`docs/shutdown-timeouts.md`](../shutdown-timeouts.md): operator reference
with directive table, default `docker stop` behaviour, configuration guidance,
and the startup-warning decision. Linked from root `README.md`; `docs/cron.md`
gains a shutdown cross-reference.

**Startup-warning decision (already implemented in code, now documented):**
warn once at startup for `supervisor` when
`process_control_timeout < supervisor.stop_timeout` (includes stock defaults);
warn for `cron` only when `cron.timeout > 0` and
`process_control_timeout < cron.timeout`; do not warn on request-serving pools
at upstream defaults. Reasoning is in the doc's "Startup warnings" section.

Regression test: `sapi/fpmng/tests/fpmng-shutdown-timeout-warnings.phpt`.

**Measured:** fpmng phpt suite on poligon (192.168.8.50, 2026-09-07):
`fpmng-shutdown-timeout-warnings.phpt` PASS; full `run-fpmng-phpt.sh` run 8/8
pass, 2 skip (fiber/session). Behaviour otherwise taken from
`docs/NOTES.md` section 3p (2026-09-06 live verification).

**Left out:** no change to defaults or to `fpm_process_ctl.c` (reference code).
