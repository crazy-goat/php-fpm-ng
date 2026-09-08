# 051 — Negative-control Laravel pools corrupt the protocol of the shared MySQL

**Priority:** medium. Test-box citizenship: the negative suite is designed to
break things, and it currently breaks them against a MySQL server other work
shares.

**Status:** open.

## Where it was seen

2026-09-08, task 025 on the test box (192.168.8.50). Runs of
`tests/frameworks/laravel/bin/run.sh` with an empty `fiber.isolate_statics`
(the designed negative controls) produced, besides the expected scenario
failures:

- MySQL client errors `RSET_HEADER packet additional data length is past 3
  bytes` — protocol corruption, i.e. garbage traffic aimed at the **shared**
  MySQL server (3306);
- worker-level failures (HTTP 502 from the gateway) when a removed
  empty-list audit ran.

The task 025 branch removed the empty-list *audit* for exactly this reason
but simultaneously added five new negative scenarios, so the exposure got
larger, not smaller. Confirmed substantive by review; recorded in
`findings.md` (2026-09-08, task 025).

## What this task must produce

1. The Laravel runner's negative suite runs against a **private** MySQL
   (`SERVICE_MODE=docker` already provisions one; make the negative phase use
   it, or document that this suite must not run in `SERVICE_MODE=external`
   against the shared box MySQL).
2. A note in the fixture README and the test-box rules section of
   `workflow.md`-level docs stating this constraint.

## Explicitly out of scope

- Making empty-list pools behave better (the corruption is the designed
  evidence that isolation matters; the goal is only to aim it at private
  infrastructure).
