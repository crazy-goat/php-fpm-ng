# 055 — Measure and improve HTTP-direct connection fairness

Status: open

## Why

The task 054 POC uses a shared libevent HTTP listener in classic/static FPM
children. The initial two-request barrier probe on the poligon failed when both
connections opened before either script started: libevent could accept both
into one worker, while another worker remained idle. Establishing the first
running request before opening the second verified independent child progress.
This is documented in `docs/http-direct.md`, "Deliberate limits".

The task 054 major-issues review confirmed that this is substantive follow-up
work, not a blocker for the explicitly experimental POC. It is kept outside the
task 054 PR as required by workflow.md.

## Scope

Measure assignment and queueing under connection bursts and keep-alive. Improve
fairness if the measurements justify it, without introducing fibers or changing
classic PHP request isolation. Do not conflate fast overload rejection with
successful PHP throughput.

## Acceptance criteria

- Report per-worker request distribution, successful throughput, error rate,
  and latency for burst and persistent-connection workloads with 1, 2, and 4
  static workers on the poligon.
- Include a workload where one worker executes a slow PHP request while peers
  are idle. Report whether queued work can progress on those peers.
- Compare the current POC with any proposed change, retaining reproducible
  configuration and raw results; report negative results too.
- Preserve request isolation, retirement, graceful reload, and master timeout
  tests. Document any scheduling limitations that remain.
