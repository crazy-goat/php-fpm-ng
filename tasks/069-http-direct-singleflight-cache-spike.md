# 069 — Spike: singleflight coalescing and worker-side response cache

Status: open
Type: spike (decision-support measurement, not a feature commitment)

## Why

A direct worker's event loop sees its whole request queue, which enables two
throughput techniques impossible through FastCGI (the intermediary hides the
queue and duplicates work): singleflight — N concurrent identical requests
execute PHP once and share the result — and a worker-local response cache that
answers without starting a PHP request at all. Both were identified as
candidate capabilities during the task 054 follow-up analysis; both carry
correctness risk (cache keying, invalidation, per-request variance) that must
be sized by measurement before any feature task.

## Questions to answer

1. Singleflight: for a thundering-herd workload (cache-miss stampede), how much
   CPU and latency does coalescing save at 1/2/4 workers on the poligon? What
   key definition is safe (method + path + normalized query + body hash?), and
   what breaks (per-user responses, non-deterministic scripts, side effects)?
2. Worker-local cache: what hit-rate assumptions are realistic, how is
   invalidation signaled (TTL only? task 072 scheduler wakeups?), and what is
   the measured cost of a cache hit versus a full PHP request?
3. Are either of these better delivered by ordinary PHP (APCu etc.) given that
   direct already lowered per-request cost, i.e. is the C-level version worth
   maintaining?

## Acceptance criteria

- Reproducible poligon measurements (task 054 harness shape, binary identity
  checks, own ports/dirs) for both techniques as throwaway patches or load
  simulations, with successful-request throughput, CPU per success, p50/p99,
  and memory cost.
- A written recommendation per technique: feature task, more measurement, or
  negative result — grounded in the numbers, including the correctness risks
  enumerated above.
- Raw artifacts retained outside the tree; summary table in the Outcome.

## Out of scope

- Merging either technique; follow-up feature tasks are separate.
- Cross-worker coherency mechanisms beyond what task 072 already proposes.
