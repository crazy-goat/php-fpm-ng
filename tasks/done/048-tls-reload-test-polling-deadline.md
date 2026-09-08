# 048 — make the tls-reload CI job robust against a slow runner

**Priority:** medium. It burns reruns and misleads whoever sees it red next.
**Status:** done. Independent of other tasks.

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

## Outcome

The flake is fixed, but **not** by the waiting strategy this task proposed. The
diagnosis in Problem above was wrong, and measuring it was what revealed the
real cause.

### The cause, measured

`fpm_http_tls_reload_master_tick()` decides a certificate was renewed by
comparing `st_mtime` in **whole seconds**
(`sapi/fpmng/fpm/fpm_http_tls_reload.c:140-142`). The script wrote
`live-cert.pem` at startup and swapped it ~0.6 s later, so whenever both writes
landed inside the same second the mtime the master looks at did not change, no
tick ever validated the new pair, and the reload never happened at all. The
symptom is identical to the one recorded above — `post-reload: connection 0
served the OLD serial` — which is why it read as a gateway that had missed its
1 s tick.

Measured on the poligon 2026-09-08 against a static-full binary
(`strings` → 6 hits for `http.tls_reload_check`):

- `main`'s script, 5 runs: **2 failed**, both with the CI symptom and **zero**
  gateway adoption notices logged. Wall time 12.8 s per passing run.
- An instrumented copy of the fixed script, 8 runs: **3 runs needed 1–2 retries**
  past an mtime collision on the first swap — 3 runs that would have failed on
  `main`.
- Fixed script, 10 runs: **10 PASS, 0 FAIL**, wall time **5.9–6.2 s**.

This establishes the mtime collision as **a** cause with a matching symptom and
a matching rate, not as the only possible one. The original hypothesis (a
gateway more than 4 s late on a loaded runner) remains **unmeasured** in both
directions.

### What was done

`build/test-http-tls-reload.sh` only; no C, no CI-workflow change.

- `swap_in()` — replaces the live pair and does not return until
  `live-cert.pem` has an mtime distinguishable from the previous one, retrying
  the copy (bounded, 50 attempts) otherwise. Used for both swaps: the same
  collision at the broken-candidate check would otherwise stall the wait for
  the rejection notice.
- `wait_for_log_count()` — one bounded wait, replacing both `sleep 4`s.
  Deadline `RELOAD_DEADLINE` = 30 × `http.tls_reload_check`, overridable with
  `FPMNG_TLS_RELOAD_DEADLINE`. Criterion 1 waits for 3 per-gateway adoption
  notices (`fpm_http_tls_reload.c:327`); criterion 3 waits for one more
  rejection notice than the count captured before its swap. It also fails
  immediately if the master exits while waiting.
- The assertions themselves are **byte-identical to before this task**:
  `assert_all_serve()` is unchanged, and each of the five criteria still
  asserts exactly what it did.

Criterion 4 is exceeded: wall time **halved** (12.8 s → 5.9 s), because 8 s of
the old runtime was the two fixed sleeps.

### Criterion 3 (the test keeps its power) — verified by injection

Not assumed, and not argued from the happy path. Two failures were injected:

- **No gateway adopts** (`http.tls_reload_check = 0`): `FAIL: post-reload:
  waited 6s for 3 log line(s) matching 'adopted reloaded TLS certificate',
  saw 0`.
- **Exactly one of three gateways never adopts** — the case this criterion
  actually names. One gateway frozen with `SIGSTOP` immediately before the
  swap, so its reload timer can never run: `FAIL: post-reload: waited 8s for
  3 log line(s) ... saw 2`. Deterministic, with the count in the message.

### The mistake this branch made first, and why the log is the readiness signal

The first implementation did what this task literally asked for: it retried the
post-reload wire assertion until it passed or the deadline expired. A review of
the branch (workflow.md step 5) showed that this **destroys the assertion**,
and the arithmetic is not marginal. The 12-sample burst is a *statistical*
assertion — 3 gateways behind `SO_REUSEPORT`, and the script cannot see which
one served a connection. With one gateway permanently stuck, a single burst is
red with probability 1 − (2/3)^12 = 99.2%, but a failing burst aborts after
~3 samples, so ~75 retries fit inside a 30 s deadline and one of them comes up
clean about **44%** of the time. Retrying would have turned the exact
regression this job exists to catch into a coin toss, while reporting PASS.

The log count has no such weakness: both notices are emitted once per process
per event, so the count is exact rather than sampled. Hence the shape above —
poll the log for readiness, then assert on the wire once, fatally. Measured
after the fix, the stuck-gateway injection is red 1 for 1 with an exact count,
where the retry version would have been ~44% green.

Also fixed from the same review: `mtime_of` failure now goes through `fail()`
instead of aborting silently under `set -e`, and the `swap_in()` comment cites
the `build-matrix.yml` run IDs from Problem above rather than citing this file
(which cited the comment back).

### Left out

- The product-side whole-second comparison is **not** fixed; changing the
  reload mechanism is out of scope here (see Out of scope above). Recorded in
  `findings.md`; it deserves its own task, which the review agreed with. A
  certificate replaced twice inside one second is currently not adopted and
  nothing is logged.
- Whether a gateway can *also* be more than 4 s late on a loaded CI runner
  remains **unmeasured**. The deadline makes the question harmless rather than
  answering it.
- `swap_in()` copies cert then key inside its retry loop, so it can re-open the
  torn-pair window that the original single `cp` pair opened once. Harmless
  here — the master needs *both* mtimes unchanged to skip
  (`fpm_http_tls_reload.c:140-142`), it re-validates on the next tick, and
  criterion 3 counts rejections from a pre-swap baseline — but it is a window,
  not an absence of one.
