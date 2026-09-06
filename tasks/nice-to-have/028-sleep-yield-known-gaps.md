# 028 — Two known gaps in the fiber sleep interception

**Track:** nice-to-have. This task only applies to `pool.executor = fiber`,
which is moving behind a build flag that is **off by default**, so a stock
binary does not contain this code at all. The `Priority:` line below is the
priority *within* the fiber track — it is not a claim against the HTTP,
cron, scheduler or proxy work, which is where the project is focused.

**Priority:** low. Neither gap makes anything worse than it was before the
interception landed; both are places where we knowingly differ from upstream.
**Status:** open.

## Context

`sapi/fpmng/fpm/fpm_pool_fiber_sleep.c` replaces the internal handlers of
`sleep()`, `usleep()` and `time_nanosleep()` for `pool.executor = fiber` pools
and suspends the request fiber on the scheduler's timer instead of blocking the
process. Measured before/after on a `sleep(3)`: 0/10 and 0/30 unrelated requests
served during the sleep, versus 10/10 and 30/30 after. Full measurement in
`docs/spike-sleep-yield-report.md`.

The interception matches upstream's argument validation and error messages
exactly, including the `time_nanosleep()` range error that upstream only
produces indirectly through `nanosleep()`'s `EINVAL`. Two behavioural
differences remain.

## Gap 1 — `time_nanosleep()` loses sub-microsecond resolution

The suspension path goes through `struct timeval`, whose resolution is
microseconds, so `tv_nsec` is divided by 1000. `time_nanosleep(0, 500)` returns
immediately in a fiber pool, where unpatched PHP sleeps 500 ns.

Nothing measured depends on this and no realistic application does, but it is a
silent difference from documented behaviour: nothing in the logs or the return
value says the sleep did not happen.

Decide and record one of: accept and document it as a known deviation of the
fiber executor, or carry the request through a nanosecond-resolution timer.

## Gap 2 — `sleep()` return value on an early wake-up

Upstream `sleep()` returns the number of seconds remaining when the sleep is cut
short. The interception returns 0 unconditionally.

Today this branch cannot be reached: nothing wakes the waiter early, because
nothing outside the sleep holds it. `time_nanosleep()` in the same file already
computes the correct remainder from a recorded deadline, so the two functions
disagree about a case only one of them can currently hit.

The risk is not present-day wrongness but a trap: whoever first adds an external
wake-up source — a cancellation, a request abort that unwinds through the
scheduler, a signal delivered to a sleeping fiber — will get a silently wrong
return value from `sleep()` and nothing will point at this file.

## Acceptance criteria

- A test asserting upstream `time_nanosleep()` resolution exists and either
  passes, or is recorded as a deliberate deviation with the reason. See
  `tasks/003-*` and the `tests/fiber-blocking/` suite for where it belongs.
- `sleep()` either returns the remaining seconds on an early wake-up, or its
  code says in one sentence why 0 is correct and what has to change if an
  external wake-up source is ever added.
- The two functions no longer disagree about the same case.

## Out of scope

Other blocking calls. `curl`, libpq, plain-file I/O and blocking `stream_select`
are separate problems with their own tasks.
