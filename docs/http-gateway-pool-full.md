# HTTP gateway pool-full policy: opt-in "wait" (issue #309)

When a gateway's shared upstream budget (`gw->upstreams_used` vs.
`gw->max_upstreams`) is exhausted, `fpm_http_pump_once()` answers a queued
request `503` + `Retry-After: 1` immediately (`FPM_HTTP_SERVICE_UNAVAIL`,
`sapi/fpmng/fpm/fpm_http.c`). That remains the default for every pool. This
document covers the opt-in alternative: instead of rejecting the instant the
budget is full, hold the request on `gw->waiting` for a bounded time and
dispatch it if an upstream frees up in time.

## Directives

```ini
http.pool_full_policy = wait      ; reject (default) or wait
http.pool_full_queue_max = 32     ; only read when policy = wait
http.pool_full_wait_ms = 500      ; only read when policy = wait
```

- `http.pool_full_policy` — `reject` (default) is the unchanged status quo.
  `wait` opts this pool into queueing.
- `http.pool_full_queue_max` — hard cap on how many requests may sit on
  `gw->waiting` at once. A request that arrives when the queue is already at
  this cap gets the ordinary immediate 503, it does not wait first. Default
  **32**.
- `http.pool_full_wait_ms` — hard cap on how long one request may sit on
  `gw->waiting`. A request still queued when this expires is rejected (503),
  the same as a full queue. Default **500**.

Both bounds are mandatory whenever `http.pool_full_policy = wait` is set;
config validation (`fpm_http_validate_pool()`) refuses startup if either is
`<= 0`, rather than silently treating a misconfigured pool as unbounded. An
unbounded queue or an unbounded wait is exactly the slowloris/hoarding hazard
the 503 exists to avoid.

## When to opt in

Only for pools whose workload is **IO-light**: request handling mostly waits
on an upstream or I/O rather than burning CPU for the whole request. There is
no runtime signal that distinguishes IO-light from CPU-bound automatically
(see `docs/spike-gateway-poolfull-report.md`) — this is an operator judgment
call, made once per pool, not something the policy infers.

Do **not** opt in a CPU-bound pool. The measurement behind this feature
(`docs/spike-gateway-poolfull-report.md`, spike #54 / run #159) found that for
CPU-bound work, queueing does not fix overload — a busy upstream is not made
to drain any faster by requests waiting for it — and it makes the client's
outcome strictly worse: `wait-large` (queue 64, wait 2000ms) at `gw=1, c=32,
cpu` eliminated rejections (145304 → 0 non-2xx) but multiplied p50 latency
7x (11.583ms → 81.015ms); at `c=128` it didn't even fully clear the overload
(143019 non-2xx remained) while still multiplying latency (17.385ms →
188.349ms). A misconfigured CPU-bound pool that opts in still has both bounds
enforced, so the damage is capped at "some requests take up to
`pool_full_wait_ms` longer, then 503 anyway" rather than unbounded — but it is
still a bad trade, and the operator should not make it.

For IO-light pools, the same report found real wins: at `gw=2, c=32, light`,
a small queue/wait (8, 200ms) all but eliminated rejection (44601 → 137
non-2xx) for +33% throughput at only +5% p50 latency; at `gw=2, c=128,
light`, a larger queue/wait (64, 2000ms) cut non-2xx from 59275 to 52 for
+45.9% throughput with p50 latency unchanged.

## Why 32 / 500ms, not the swept values

Run #159 swept exactly two combinations — `wait-small` (queue 8, wait 200ms)
and `wait-large` (queue 64, wait 2000ms) — chosen to bound that measurement,
not to be shipped verbatim as a default; issue #309 explicitly asks for a
real default distinct from both.

The shipped default (queue 32, wait 500ms) sits inside the measured range on
purpose:

- **Queue depth 32** matches `pm.max_children`'s common small-pool value used
  throughout the #159 matrix and sits between the two swept queue depths (8,
  64). It gives an IO-light burst real headroom (more than `wait-small`'s 8,
  which measurably still left some cells short of `wait-large`'s near-total
  rejection elimination) without the extra memory/connection footprint of
  holding 64 requests open per gateway at all times.
- **Wait bound 500ms** is deliberately closer to `wait-small`'s 200ms than to
  `wait-large`'s 2000ms. The report's CPU-bound numbers show the wait bound is
  the actual damage cap for a misconfigured pool: at `wait-large` (2000ms)
  the p50 for CPU-bound work rose to 81–188ms, i.e. well under the 2000ms
  ceiling in practice, but the ceiling itself is what a client sees in the
  worst case (a request that never gets a slot). 500ms keeps that worst case
  close to what a well-behaved client already tolerates from the existing
  `Retry-After: 1` reject-and-retry path (one retry round trip), while still
  being long enough for the IO-light `light` workload cells — whose p50s were
  all under 5ms even under load — to clear a transient budget shortage
  comfortably inside the bound.

These are conservative, safe-by-default numbers for a first shipped
implementation, not the result of an independent sweep at 32/500 specifically
— an operator tuning for a specific deployment should measure their own
workload with `build/benchmark-http-gateway-poolfull.py` rather than assume
these numbers transfer unchanged.

## Tuning for your deployment

Start from the shipped defaults and adjust based on what you're actually
seeing:

- If you still see occasional 503s under a load spike you believe is genuinely
  transient and IO-light, raise `http.pool_full_queue_max` before raising
  `http.pool_full_wait_ms` — a deeper queue absorbs more concurrent requests
  at the same per-request wait cost, whereas a longer wait bound raises the
  worst-case latency every queued request can incur.
- If p50/p99 latency under load is creeping up more than your clients can
  tolerate, lower `http.pool_full_wait_ms` first. A shorter bound converts
  more overload into fast 503s instead of slow successes — the same
  reject-vs-wait trade-off `docs/spike-gateway-poolfull-report.md` measured,
  just re-tuned toward the reject side without turning the policy off
  outright.
- If your workload's CPU-boundedness is uncertain, don't opt in at all and
  measure with the harness first. The data above shows there is no
  configuration of `wait` that helps a CPU-bound pool — only `reject` does.

## Observability

A response that spent time on `gw->waiting` under the `wait` policy carries
an `X-Fpmng-Queue-Wait: <ms>` response header reporting how long it actually
waited (measured from enqueue to the moment it was handed to an upstream, not
including the request's own runtime). The header is absent from responses
that never queued.
