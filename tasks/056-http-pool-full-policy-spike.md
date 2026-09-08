# 056 — Spike: pool-full policy for the HTTP gateway — measure before choosing

Status: open
Type: spike (decision-support measurement, not a feature commitment)

## Why

Benchmarking for task 054 (docs/http-direct.md, `build/benchmark-http-direct.py`)
exposed that `pool.type = http` under `wrk -c 32` with `pm.max_children = 4`
returns massive 503 counts (714k/813k non-2xx across three 10-second runs) while
nginx + FastCGI on the same workload queues excess requests in the kernel backlog
and answers all of them. The gateway's fast-fail is deliberate
(`sapi/fpmng/fpm/fpm_http.c:13-19`, `http-pool-full-503.phpt`): a kept FastCGI
connection pins one worker, the shared budget is `pm.max_children`, and overflow
is rejected with 503 + `Retry-After` (`fpm_http.c:153-155`).

Today we cannot honestly compare alternatives: there is no queuing variant to
measure, and raw RPS at overload mostly measures the 503 generator, not served
requests. This spike fills that gap.

## Candidate variants to compare

1. **reject (status quo)** — current fast-fail 503 + `Retry-After`.
2. **wait** — queue requests in the gateway until a pinned worker frees up,
   with an explicit queue cap and timeout (both must exist; unbounded queue
   is a slowloris/hoarding hazard).
3. **idle_timeout tuning** — status quo with more aggressive release of pinned
   workers (config-only, no code).
4. **fiber executor** — existing 128-connection budget (`fpm_pool_type.c:60`),
   build-flag gated, for reference only.

## Acceptance criteria

- A reproducible poligon run using the task 054 harness shape (binary identity
  check, response-body verification, own ports/dirs) comparing the variants on:
  light and CPU-bound scripts, concurrency 4, 32, and 128, with
  `pm.max_children = 4`, `http.gateways = 1` and `= 2`.
- Report per variant: successful (2xx) req/s, rejection counts, p50/p99 of
  successful responses, CPU per successful response, and queue-wait time if
  variant 2 is implemented even as a throwaway patch.
- A written recommendation which variant (if any) is worth a real task, with
  the reasoning grounded in the measured numbers, including a negative result
  if the status quo wins.
- Raw artifacts retained outside the tree (findings or scratch), summary table
  in the task Outcome.

## Out of scope

- Merging any policy change into the gateway as part of this spike; the
  follow-up task (if any) is separate.
- Changes to http-direct; its congestion behavior is task 055.
