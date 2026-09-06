# 029 — Investigate intermittent GH-16432 FPM status failure

**Priority:** high. This is the only unresolved failure observed while running the
150-test upstream FPM suite and may indicate a resource-sensitive regression.
**Status:** open.

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
recorded in [`docs/fpm-phpt-results.md`](../docs/fpm-phpt-results.md). The
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
