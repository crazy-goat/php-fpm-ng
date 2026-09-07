# 048 — make the tls-reload CI job robust against a slow runner

**Priority:** medium. It burns reruns and misleads whoever sees it red next.
**Status:** open. Independent of other tasks.

## Context

`build/test-http-tls-reload.sh` (task 040's CI job) verifies that a swapped
certificate reaches every gateway process without a restart. After the file
swap it does a fixed `sleep 4` (`build/test-http-tls-reload.sh:180-183`,
margin = master check + each child's 1 s timer) and then `assert_all_serve`
fails on the **first** connection whose serial does not match
(`build/test-http-tls-reload.sh:81-82`).

## Problem

On a slow or shared CI runner, one of the `http.gateways = 3` children can
miss its reload tick within that margin (its 1 s timer competes with the event
loop that has just served the 300-request load loop). The job then fails even
though the reload mechanism works — it just needed a second or two more.

Measured 2026-09-07: the job failed 5 times across ~28 runs of
`build-matrix.yml`, on four unrelated branches — task/005 (runs 34139815139
twice, 34140641442; all "post-reload: connection 0 served the OLD serial"
right after the sleep), task/003 (34121726613), task-031 (34120910345,
34120774897). task-031 passed the same test at 12:52 with no code change, and
task/005 passed on the third rerun — both after wasting investigation time.
Task/005's three failures triggered a full A/B (pure-main dispatch green,
pre-fix commit red) before the flake was identified as unrelated to the code
under test: in the canonical CI build every patch-0007 hunk is compiled out
(`#ifdef HAVE_FPMNG_FIBER_TLS`), so the binary under test was functionally
identical to main's.

The same fixed-sleep pattern appears at the broken-candidate check
(`build/test-http-tls-reload.sh:200-204`).

## Acceptance criteria

1. After the swap, the script polls until every gateway serves the new
   serial, within a bounded deadline (an order of magnitude above the
   reload-check interval, not a tuned-to-the-runner constant). Only exceeding
   the deadline fails the test — being slow does not, being stuck does.
2. The broken-candidate check (criterion 3 in the script) gets the same
   treatment: poll until the state settles, bounded.
3. The deadline is generous enough that a healthy reload on a slow runner
   still passes, but a reload that never lands fails — the test keeps its
   power to catch a real regression (a gateway that never adopts the new
   generation must still produce a red job).
4. On the poligon (fast machine) the full script still passes end to end,
   and the wall time does not grow for the healthy case (no new fixed sleeps
   added on the happy path).
5. No change to what the job *asserts*: the five criteria in the script stay
   as they are. This task is about waiting strategy only.

## Out of scope

- Any change to the reload mechanism itself (`fpm_http_tls_reload.c`).
- Making `assert_all_serve` reason about *which* gateway served a connection
  (the script cannot see pids behind SO_REUSEPORT; serial equality is the
  observable contract).
