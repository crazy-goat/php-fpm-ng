# 063 — Connection limits and slowloris hardening for HTTP-direct

Status: open
Depends on: 055

## Why

The task 054 POC bounds per-worker memory (pending responses ≤ 16, request
buffers) but has no deliberate total-connection policy: connections are cheap
to hold, which is a strength under keep-alive and an attack surface under
connection hoarding and slow request bodies (slowloris). Task 055 addresses
fairness of assignment; this task addresses deliberate admission and body-read
policy on top of that result.

## Scope

A validated, documented connection policy for direct pools: a configurable
total connection limit (per pool and/or per worker), per-client connection
limits, explicit behavior at the limit (close vs 503 vs queue with bound), and
hardening of slow-body and slow-header clients beyond the existing
`http.read_timeout` (which today covers incomplete requests). Whatever is not
implemented must fail validation, keeping the task 054 rule.

## Acceptance criteria

- Data-asserting tests for: limit reached (documented refusal), per-client cap
  enforced, slow-header and slow-body clients dropped within documented
  timeouts, and normal keep-alive traffic unaffected by the limits.
- Measurement on the poligon (task 054 harness shape) showing the limit
  protects successful throughput under a hoarding workload without hurting the
  baseline; negative results recorded.
- Interaction with graceful drain (SIGQUIT) tested: draining does not wedge on
  connections the policy would refuse.
- Documentation in `docs/http-direct.md` describing the policy, defaults, and
  what is deliberately absent.

## Out of scope

- Fair assignment across workers (task 055).
- Global (cross-worker) limits via shared state — that is the task 072
  scheduler spike; per-worker limits here must not preclude it.
