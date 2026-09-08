# 066 — Per-connection metrics and scoreboard extension for HTTP-direct

Status: open
Depends on: 055

## Why

The task 054 benchmark could only observe worker CPU, RSS, and end-state
scoreboard counters; it could not see queue depth, accept distribution, or
rejection reasons, which is exactly what made the fairness finding (task 055)
hard to quantify from the outside. The worker's event loop already knows all
of this. Exposing it makes every future performance task measurable without
external instrumentation.

## Scope

Extend the direct transport's internal accounting — per-worker: current
connections, queued (accepted, not yet executing) requests, in-flight PHP
requests, rejections by reason (buffer bound, pending cap, limits from task
063), accept counts — and expose it through the existing scoreboard/status
surface for direct pools, with stable field names and a documented schema
version. No new process or endpoint style; reuse what task 061 delivers for
status paths.

## Acceptance criteria

- Status output of a direct pool reports the new fields; a data-asserting test
  generates known load and verifies the counters move in the right direction
  (queue depth during a slow request, rejection reasons under a hoarding
  client).
- Counters are consistent across graceful reload/stop (documented reset
  semantics) and survive worker recycling by `pm.max_requests`.
- The schema is documented with a version marker so tooling can detect changes.
- The task 055 measurements are reproduced using only these counters (no
  external polling), demonstrating the observability goal.

## Out of scope

- Time-series storage, Prometheus exporters, or JSON dashboards; a follow-up
  task if operators want them.
- Metrics for `http`/`fastcgi` pools beyond what exists today.
