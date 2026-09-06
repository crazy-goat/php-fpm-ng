# 029 — Investigate intermittent GH-16432 FPM status failure

**Priority:** high. This is the only unresolved failure observed while running the
150-test upstream FPM suite and may indicate a resource-sensitive regression.
**Status:** done.

## Context

Task 001 ran the upstream test
`sapi/fpmng/tests/gh16432-status-high-nprocs.phpt`, whose purpose is to verify
that `fpm_get_status()` remains safe with `pm.max_children = 12800`.

The measured polygon run used the same PHP source checkout and the same
`php-fpm-ng` binary for three complete 150-test suite runs:

- two complete runs reported the test as `PASS`;
- one complete run reported `FAIL`;
- five isolated repetitions of the test reported `PASS`.

The failing run did not show a segmentation fault. The PHPT harness reported
that the expected startup notice did not match:

```text
ERROR: The NOTICE does not match expected message

LOGS:

Done
```

The exact source, binary fingerprints, raw output and result summaries are
recorded in [`docs/fpm-phpt-results.md`](../../docs/fpm-phpt-results.md). The
polygon result directories were under
`/home/piotr/rd/tasks/001-fpm-phpt-run-20260906T171646Z`.

## Problem

The evidence does not yet distinguish between:

- a `php-fpm-ng` defect that appears only after the preceding suite workload;
- an upstream test or logging assumption that is sensitive to process/resource
  state; or
- an environment or test-order artifact on the polygon.

The isolated 5/5 result is not enough to call the full-suite failure harmless,
and the single full-suite failure is not enough to call it a stable regression.

## Acceptance criteria

1. The intermittent result is reproduced, ruled out with documented controls, or
   narrowed to a clearly identified environmental or ordering condition. The
   evidence must include complete-suite and isolated-test results using explicit
   source and binary fingerprints.
2. The missing or unexpected startup notice is explained from the captured FPM
   logs and process lifecycle, rather than dismissed as a generic PHPT failure.
3. A comparable upstream FPM control is measured from the same PHP source
   revision, or the reason it cannot be run is recorded as `NOT MEASURED`.
4. The result receives exactly one verdict: **our bug**, **intended difference**,
   or **test artifact**. If it is our bug, the fix and regression coverage are
   linked from this task; if it is an intended difference or artifact, the
   relevant documentation and assumption are named.
5. The upstream `.phpt` file and its expected output are not weakened, deleted,
   forked or converted into a skip to make the result green.
6. The final record includes the bounded command, test order or workload needed
   to trigger the result, resource limits, source revision, binary paths and
   SHA-256 fingerprints.

## Constraints

Use an isolated polygon directory and dedicated resources. Keep all reruns
bounded, preserve raw logs, and do not use `pkill php-fpm`, `FLUSHDB` or
`FLUSHALL`. A failure that cannot be reproduced remains documented as an
unresolved observation until the controls above provide a verdict.

## Relation to task 001

Task 001 is complete because the upstream suite is runnable and all 150 results
are recorded. This task owns the separate investigation of the one intermittent
`gh16432-status-high-nprocs.phpt` observation; it does not change the measured
150-test report retroactively.

## Outcome — 2026-09-06

**Verdict: our bug.**

### Root cause and fix

`php-fpm-ng` initialized the application-metrics region before it announced
readiness. The default `fpmng_metrics.series_limit = 256` reserves one table of
entries for every configured worker slot. With `pm.max_children = 12800`, the
region is about 3.2 GiB (`fpmng_metrics_shm_size()` is approximately
`16 + 12800 * (8 + 256 * 1048)` bytes). `fpmng_metrics_shm_init()` then called
`memset(shm, 0, size)`, faulting in and clearing the entire mapping before the
`fpm is running` and `ready to handle connections` notices were emitted.

`fpm_shm_alloc()` uses a fresh `MAP_ANONYMOUS | MAP_SHARED` mapping, which is
already zero-filled lazily. The fix removes only this eager whole-region
`memset`; the header is still initialized explicitly and entries are still
cleared when allocated. No metrics API or upstream test was changed.

### Measurements

All follow-up runs used the isolated polygon directory
`/home/piotr/rd/tasks/029-fpm-phpt-followup-20260906T200000Z` on Linux `lenovo`,
`7.0.0-31-generic`, `x86_64`. The source tree was PHP commit
`2126f2d4377cbbf4ffc2e7755a392e048d2ce963` (the fixture commit on upstream base
`8c7a64ef51eebf253ab43eee3e24d0b9c4c79c5f`). Each PHPT run used
`TEST_FPM_RUN_AS_ROOT=1`, `TEST_FPM_TIMEOUT=60`, the shell's default limits
(`RLIMIT_NOFILE=1024`, `RLIMIT_NPROC=123651`, unlimited address space), and a
dedicated result directory. The raw logs, TSV statuses, commands and helper
scripts remain under that polygon directory.

The original Task 001 binary was CLI SHA-256
`ad3e97ce66ba1716de14b168f8a1b5b8e9b88de10153a1447c15979ee979eeb5`, FPM SHA-256
`738f0f9d66ac4fe1462cd2c763ffa6968a581771338de2441dc3f1353d36ec27`. The
fixed binary was CLI SHA-256
`e79ffa677a84b96f0b58084b6d76bbcaabc196f3231bfd6042fb3651e4570bcd`, FPM SHA-256
`d1aceabb53f5875bb816ac034830c3c52dc0bc38ac3f10b1022329c57d4f807b`; its
`strings` fingerprint contained `fpmng_`, `pool.type` and `fastcgi-ng`. The
clean upstream control built from the same source revision was CLI SHA-256
`3034a52ba71d59b5fc0b1cdf8d5ac456f990acb1b7bdce0498bc80b902fd7757`, FPM
SHA-256 `84cd3d0fc40edaf6f27a5468ddb14e5f151953514427b7f96b7e721f4f4cb137`.

The bounded full-suite command was, for each `N = 1, 2, 3`:

```sh
BASE=/home/piotr/rd/tasks/029-fpm-phpt-followup-20260906T200000Z
TEST_PHP_EXECUTABLE="$BASE/fpmng-fixed-build/sapi/cli/php" \
TEST_PHP_FPM_EXECUTABLE="$BASE/fpmng-fixed-build/sapi/fpmng/php-fpm-ng" \
TEST_FPM_RUN_AS_ROOT=1 TEST_FPM_TIMEOUT=60 \
"$BASE/run-fpm-phpt.sh" \
    "$BASE/php-src-fixed" "$BASE/results/fixed-full-N"
```

The equivalent original-binary control used the Task 001 CLI/FPM paths and
`$BASE/php-src` with `results/full-N`. The upstream control ran the unchanged
`sapi/fpm/tests/gh16432-status-high-nprocs.phpt` from
`$BASE/upstream-php-src` against `$BASE/upstream-build/sapi/fpm/php-fpm` with
the same `run-tests.php` options and timeout.

Results:

- The historical observation remains two full-suite passes and one failure,
  followed by five isolated passes. The failing output was only
  `ERROR: The NOTICE does not match expected message` with empty `LOGS`; no
  segmentation fault was observed.
- Three fresh full-suite runs with the original binary all passed
  `gh16432-status-high-nprocs.phpt`; three fresh isolated runs and five
  `gh15395 -> gh16432` order controls also passed.
- The fixed binary passed the test in five isolated runs in 0.125–0.127 s and
  passed it in all three full suites. The suite totals were 133/1/1/15 and
  132/1/2/15 for PASS/FAIL/ERROR, WARN and SKIP; the remaining failure is the
  already documented intended `http-basic.phpt` difference, and the WARN
  variation is the upstream XFAIL test.
- The upstream control passed 5/5 isolated runs in 0.124–0.134 s.
- A direct startup measurement with `pm.max_children = 12800` reported
  2.782 s and `VmRSS = 3,373,784 kB` for the original binary, versus 0.063 s
  and `VmRSS = 20,036 kB` for the fixed binary. Both mappings had the same
  `VmSize = 3,558,364 kB`, confirming that the fix avoids touching the
  reserved mapping rather than changing the metrics geometry.
- As a harness control, a bounded wrapper that delayed FPM startup by four
  seconds reproduced the same notice mismatch and empty `LOGS`. This explains
  the captured failure: `FPM\Tester::expectLogStartNotices()` has a three-second
  log-read timeout, so a resource-delayed startup is reported as a notice
  mismatch rather than as a startup error. The captured FPM lifecycle and the
  memory measurement identify the delay as the metrics initialization in our
  binary, not an upstream test assumption or a segfault.

The existing upstream `gh16432-status-high-nprocs.phpt` remains the regression
coverage and was not weakened, deleted, forked or converted to a skip. This task
is complete with the one-line metrics initialization fix and the full-suite,
isolated and upstream-control measurements above.
