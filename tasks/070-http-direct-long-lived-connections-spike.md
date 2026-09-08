# 070 — Spike: long-lived connections (SSE, long-polling, WebSocket) and `fpm_push()`

Status: open
Type: spike (decision-support analysis and prototype, not a feature commitment)

## Why

Long-lived connections are the capability class that FastCGI-based pools
structurally cannot offer PHP: a request lasting minutes holds a worker with
no way to interleave, and an intermediary has to buffer everything. HTTP-direct
removes the intermediary, but the classic executor still runs one request at a
time per worker — so the open question is how far long-lived connections can go
without fibers, and exactly where fiber execution becomes mandatory. `fpm_push()`
(writing to a connection from outside its request, e.g. from another worker or
a timer) depends on the same answer.

## Questions to answer

1. Classic executor, one slow SSE stream per worker: what throughput do the
   other connections on that worker see, measured (this quantifies the known
   head-of-line effect rather than assuming it)?
2. Streaming (task 058) plus `fpm_respond()` (task 059): how much of the SSE /
   long-polling use case do they cover without any concurrency, and what
   remains broken (fan-out from other requests, timers firing while PHP runs)?
3. `fpm_push()` driven by the task 072 scheduler layers (I/O and cross-worker
   wakeup, no fibers): what works — pushing pre-computed bytes to an open
   connection — and what does not — generating content on demand?
4. Compatibility assessment: does adding a fiber executor for direct pools
   later conflict with anything proposed here (expected answer: no, it is the
   natural completion; verify, do not assume)?

## Acceptance criteria

- Measured answers to questions 1-3 on the poligon with reproducible scripts
  and raw artifacts; a clear written boundary "possible without fibers /
  requires fibers" per use case.
- A compatibility verdict for fiber-plus-direct (question 4) with reasoning
  from the code, not assumption.
- A recommendation: which long-lived features, if any, deserve feature tasks,
  in what order relative to tasks 058/059/072; negative results recorded.

## Out of scope

- Implementing long-lived support as a supported feature.
- Changing the fiber executor itself or its build gating.
