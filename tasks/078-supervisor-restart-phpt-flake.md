# 078 — `fpmng-supervisor-restart.phpt` intermittently reports `restarts=0`

Status: todo
Type: bug
Depends on: —
Related: 017 (process_control_timeout warnings for supervisor pools)

## Why

`sapi/fpmng/tests/fpmng-supervisor-restart.phpt` failed once in CI with

```
FAIL: supervisor restarts=0
```

Run 34233631481, job 102087639705 (the `fpmng-phpt` job of PR #40). Rerunning
the same job on the same commit passed (job 102089672363), and the test failed
in **1** of the last 40 `build-matrix.yml` runs — the only other `fpmng-phpt`
failures in that window were different tests (`fpmng-http-direct-worker-opcache.phpt`
BORKED in run 34226184469, `fpmng-http-listen-default.phpt` FAILED in run
34125730809). So: one traceable occurrence, reproducibility unknown.

The number matters more than the frequency. The test already polls to a 15 s
deadline before giving up:

```
$deadline = time() + 15;
while (time() < $deadline) {
    if (is_file($counter) && (int) file_get_contents($counter) >= 2) { break; }
    usleep(200000);
}
```

with `supervisor.processes = 1`, `supervisor.restart = always`,
`supervisor.restart_delay = 1`. Two cycles need ~2 s of the 15 s available, so
`restarts=0` is not a tight margin being missed — it means the supervised
script did not run to completion **even once** in fifteen seconds, on a pool
whose entire job is to run it. Raising the timeout would hide that, and is the
wrong fix.

There is no evidence in CI to say which of these happened, because the
`fpmng-phpt-results` artifact for the failing run (artifact 10059148802)
contains only `run.log`, `results.tsv`, `test-output.log` and the build
environment — no FPM error log. The test itself contributes to that: on the
failure path it prints one line and `exit(1)`s without reading `{{FILE:LOG}}`,
so the master's own account of what it did with the child is discarded exactly
when it is needed.

## What

Find out why the supervised script can fail to run once in 15 s, and fix that.
Separately, make a future failure diagnosable from the CI artifact alone.

## Acceptance criteria

1. A written cause: what the master did instead of running the script, backed
   by a reproduction (a stress loop, an induced delay, or an instrumented
   build) or, if it cannot be reproduced, by a `file:line` argument for a
   specific race with the evidence that supports it. "Probably slow CI" is not
   an answer to this criterion — the 15 s budget is the reason why.
2. If the cause is in the product, it is fixed and a test covers it. If the
   cause is in the test harness (counter path, startup-notice synchronisation,
   the temp directory, the script the Tester writes), the harness is fixed and
   the product is stated to be correct, with the reasoning.
3. On failure the test prints the pool's error log, and enough state to tell
   "the child never started" from "the child started and never wrote the
   counter" — for example whether the counter file exists at all, and whether
   the master logged a spawn. Verified by making the test fail on purpose
   (point the supervisor at a script that exits non-zero without writing) and
   showing the output identifies which case it is.
4. The test passes 50 consecutive local runs after the change, and the number
   is recorded in the Outcome. If the cause was a race whose window the fix
   narrows rather than closes, say what remaining rate the 50 runs can and
   cannot rule out.
