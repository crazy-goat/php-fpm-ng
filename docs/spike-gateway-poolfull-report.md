# Spike #54: pool-full policy for the HTTP gateway

Status: DONE. Measurement run: #159 (five harness invocations on
`piotr@192.168.8.50`, `~/rd/run{1,2,3,4,5-satcheck}`, raw artifacts under
`findings-benchmark-artifacts-054/issue-159-measurement-run/` — location
recorded on #54). Numbers below cite `report.txt` in that directory, which
`aggregate.py` (same directory) generates from the four `results.json` files.

## Question

`fpm_http_pump_once()` answers `503` + `Retry-After: 1` the instant a
gateway's shared upstream budget is exhausted
(`sapi/fpmng/fpm/fpm_http.c:1394-1406`, `FPM_HTTP_RETRY_AFTER`,
`fpm_http.c:176`). Is that status quo the right pool-full policy, or does
one of "wait" (#155), "reclaim" (#156), `idle_timeout` tuning (#157), or a
different connection budget entirely (the fiber executor, for reference) do
better — and by how much, under a client that actually pays the cost of a
rejection instead of firing the next request the instant a 503 arrives
(#158)?

## Verdict

**No variant replaces the status quo as the default. "Wait" is worth a
real task, but only as an opt-in policy scoped to IO-light pools — not as a
new default, and not for CPU-bound pools.**

- The premise that made rejection look free was itself measured and found
  materially smaller once a client honours `Retry-After` and retries
  (#158). At `gw=1`, `c=32`, `cpu` workload, the retry client's per-attempt
  p50 (10.232ms) and end-to-end p50 (10.234ms) are within noise of each
  other — only `attempts_per_success = 1.03` extra attempts were needed at
  the median. The tail is where the cost actually lands: p99 end-to-end
  jumps to 429.515ms against a 10.376ms per-attempt p99, a ~41x difference
  (`reject-retry-gw1-c32-cpu`, `report.txt`). So the status quo's real cost
  to a well-behaved client is concentrated in the tail, not the typical
  case — a materially weaker case against it than #54's opening comparison
  (`docs/http-direct.md`'s "5 015 successful req/s against 714 509
  non-2xx", cited in #158) suggested, because that comparison used a
  client that never paid any cost for a rejection at all.
- **"Wait" (#155) — real throughput and rejection-count wins, but only for
  IO-light work, and at a latency cost that scales with how long the
  workload actually holds a slot.** At `gw=2`, `c=32`, `light`,
  `wait-small` (`FPMNG_GW_WAIT_QUEUE_MAX=8`, `FPMNG_GW_WAIT_MS=200`) all
  but eliminates rejection (non-2xx 44 601 → 137) and lifts throughput
  +33% (16 527.7 → 21 966.4 req/s), for a p50 latency cost of +5%
  (1.245ms → 1.309ms) (`reject-gw2-c32-light`, `wait-small-gw2-c32-light`,
  `report.txt`). `wait-large` (queue 64, 2000ms cap) does the same again
  more completely at `gw=2`, `c=128`, `light`: non-2xx 59 275 → 52, rps
  +45.9% (15 634.9 → 22 808.2), p50 latency unchanged within noise (4.764ms
  → 4.757ms) (`reject-gw2-c128-light`, `wait-large-gw2-c128-light`,
  `report.txt`).
  For `cpu` workload the same policy is a bad trade: `wait-large` at
  `gw=1`, `c=32`, `cpu` fully eliminates rejection (145 304 → 0 non-2xx)
  but multiplies p50 latency by 7x (11.583ms → 81.015ms, queue-wait p50
  70ms) (`reject-gw1-c32-cpu`, `wait-large-gw1-c32-cpu`, `report.txt`) —
  and at `c=128` it does not even fully clear the overload (143 019
  non-2xx remain, barely down from 146 455) while still multiplying
  latency (17.385ms → 188.349ms) (`reject-gw1-c128-cpu`,
  `wait-large-gw1-c128-cpu`). CPU-bound children cannot drain a queue any
  faster than they already process requests, so queueing does not fix
  overload there — it only hides the 503 behind a growing wait, which is a
  worse outcome for a client than an instant, bounded-cost rejection.
- **"Reclaim" (#156) — real but narrow: only shows up with `gw ≥ 2` on
  IO-light work.** At `gw=1` it has nothing to steal from and tracks
  `reject` almost exactly (`reject-gw1-c32-cpu` 384.3 req/s vs
  `reclaim-gw1-c32-cpu` 383.6 req/s). At `gw=2`, `light`, it recovers
  meaningfully: `c=32` non-2xx 44 601 → 34 107 (−23.5%), rps +6.5%
  (16 527.7 → 17 598.2), mean held budget 3.92 → 0.00 — i.e. the stranded
  budget #157 measured is the thing actually being freed
  (`reject-gw2-c32-light`, `reclaim-gw2-c32-light`, `report.txt`). Under
  `cpu` workload at any gateway count, there is no idle upstream to steal
  (workers stay busy), so reclaim tracks `reject` closely there too
  (`reclaim-gw2-c32-cpu` 388.1 req/s vs `reject-gw2-c32-cpu` 387.3 req/s).
  A real win, but one that only pays off for multi-gateway, bursty/IO-light
  deployments — narrower than "wait"'s.
- **`idle_timeout` tuning (#157) — dropped.** #157's own verdict (posted
  as an issue comment, referenced from #159's PR #304) found no workload
  where tuning `http.idle_timeout` moved the stranded-budget problem; #159's
  matrix accordingly ran no cells for it, per #159's own stated condition
  ("only if #157 found a workload where it can move"). Recorded here as
  dropped, not silently absent, per this issue's acceptance criteria.
- **Fiber executor — reference only, not a policy, out of scope for this
  recommendation.** A 128-connection budget instead of `pm.max_children`
  (`fpm_pool_type_http_concurrent_init()`, `sapi/fpmng/fpm/fpm_pool_type.c:60`)
  eliminates rejection everywhere it was measured — `n_non2xx = 0` in
  every `fiber` row of run1 — and even lifts `cpu` throughput at `c=128`
  (395.6 req/s vs `reject`'s 318.1, +24.3%). But the cost moves from an
  explicit, bounded 503 to unbounded tail latency for CPU-bound work: p99
  at `gw=1`, `c=128`, `cpu` is 3054.360ms against `reject`'s 21.830ms — a
  ~140x tail cost, because 128 fibers contending for a handful of real CPU
  cores just serializes the work instead of signalling overload
  (`fiber-gw1-c128-cpu`, `reject-gw1-c128-cpu`, `report.txt`). It also
  needs its own build (`--enable-fpmng-fiber`, off by default,
  `sapi/fpmng/config.m4:549-566`) and a fundamentally different execution
  model with its own constraints (no ZTS, no OPcache — see
  `docs/spike-sleep-yield-report.md`'s "Risks" section). This confirms
  #54's own framing of it as "for reference only", not a candidate for
  this decision.

## Recommendation

**Open a real task for an opt-in "wait" pool-full policy, gated to pools
whose workload is IO-light** (the operator states this, there is no
runtime signal in this data that distinguishes the two automatically), with
these constraints carried over from the measurement:

- Off by default. The status quo remains the default policy for every
  pool.
- Not offered for pools the operator has reason to believe are CPU-bound —
  the data shows a queue does not fix CPU-bound overload and actively makes
  the client's outcome worse (higher latency, and at high concurrency,
  rejection isn't even avoided).
- `queue_max` and the wait bound need a real default worth shipping and a
  documented tuning guide, not two arbitrary swept values — that is
  implementation work for the follow-up task, not this spike.
- `reclaim` (#156) is a smaller, separate candidate: real value for
  multi-gateway, IO-light deployments, but narrow enough (no effect at
  `gw=1` or under CPU load) that it does not need to block or bundle with
  the "wait" task. Worth its own follow-up issue if multi-gateway
  deployments of this shape turn out to be common — not filed yet, since
  no such deployment has been named.

Neither #155's `spike/155-wait-policy` branch nor #156's
`spike/156-reclaim` branch is merged; both stay throwaway per #54's scope,
which puts merging a policy change out of scope for the spike itself. If
the follow-up task above is opened, it starts a new implementation rather
than merging either spike branch, since both were built to demonstrate
feasibility and numbers, not to be production code (see each issue's own
closing comment).

## What was measured, and where

- Harness: `build/benchmark-http-gateway-poolfull.py` (#154, merged in PR
  #304), extended with the retrying-client scenario (#158) and the shared
  upstream-budget sampler fix (#157, PR #304).
- Matrix: `gateways ∈ {1, 2}` × `concurrency ∈ {4, 32, 128}` ×
  `workload ∈ {light, cpu}` × `arm ∈ {reject, reclaim, fiber, wait-small,
  wait-large, reject-retry}`, `pm.max_children = 4`, 3 rounds each, plus a
  one-off thread-sweep saturation check at `c=128`. Full acceptance-criteria
  bookkeeping (metadata, body verification, box-clean, port ranges used) is
  in the #159 closing comment.
- 216 measured cells (108 + 36 + 36 + 36), all `body_check.ok = true`.
- Two known gaps, disclosed rather than hidden: the harness has no
  per-gateway 503 attribution for any binary tested (`upstreams.
  per_gateway_503` is `null` everywhere, with the reason recorded in the
  row), and no discrete "N reclaims happened" counter — reclaim's effect
  is only visible indirectly, through the non-2xx count and the
  `budget_held_total` delta against `reject` at the same cell.

## Out-of-scope discoveries

None found in this spike that were not already covered by an existing
issue (#157's stranded-budget finding was already filed before this run;
the retry-client tail-latency asymmetry is the substance of this report's
recommendation, not a separate discovery).
