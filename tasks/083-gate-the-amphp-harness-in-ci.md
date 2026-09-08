# 083 — Decide whether the amphp harness is gated in CI or declared optional

Status: open
Type: decision + build
Related: `build/test-http-direct-amphp.sh`, `.github/workflows/build-matrix.yml`

## Why

`build/test-http-direct-amphp.sh` needs Composer, and the `--disable-all` CLI
we build has no phar, so `vendor/` has to come from somewhere else. The harness
SKIPs cleanly instead — measured on the test box: *"PHP's phar extension is
missing"*. The consequence is that task 074's amphp integration, one of the two
userland bridges this SAPI exists to support, is untested on every commit and
nothing says so out loud.

Confirmed as task-worthy twice: by the task 073 branch review and again by the
task 079 review, and left in the gitignored `findings.md` without a task file
since 2026-09-08.

## Scope

Decide, and write the decision down before implementing:

1. **Gate it.** Build a second CLI with phar (or fetch a pinned
   `composer.phar`) in the CI image, or commit/cache a pinned `vendor/`.
2. **Declare it optional.** Keep the SKIP, but make the harness say in one line
   why it skipped and make that visible in the job summary rather than only in
   a log, so "green" does not silently mean "not run".

Option 1 is the more useful of the two if the cost is a cached `vendor/`; the
point of the task is to stop the current state, which is option 2 without
anyone having chosen it.

## Acceptance criteria

- The decision and its cost are written down here first.
- If gated: the job fails when the amphp bridge breaks, demonstrated by
  breaking it once on a scratch branch.
- If optional: a skip is impossible to mistake for a pass when reading the CI
  run, and `examples/http-direct-worker/README.md` says the integration is
  covered by a manual harness.

## Out of scope

- The Docker Compose harnesses (`test-http-direct-worker-mysql.sh`,
  `test-http-direct-worker-react.sh`); they need a Docker runner, which is a
  separate question from phar.
