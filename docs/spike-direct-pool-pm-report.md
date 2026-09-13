# Spike #66: dynamic and ondemand process managers for HTTP-direct

Status: DONE. Measurement run: #169 (one run, one box, one binary, poligon
`192.168.8.50`, `/home/piotr/issue169-run/`, 608 rows — outcome comment on
#66). Raw artifacts also mirrored at
`findings-benchmark-artifacts-066/issue-169-measurement-run/` (untracked,
`results.json` sha256 `901ba969b70dfabf1712281f66db74f1aa250dc1edc22994c81f7028f67ea7eb`,
verified against the poligon copy). Harness: `build/benchmark-http-direct-pm.py`
(#163); decision rule fixed before any measurement in
`build/benchmark-http-direct-pm.md` (#163).

## Question

`pool.type = http-direct` currently refuses `pm = dynamic` and `pm = ondemand`
for both direct executors
(`sapi/fpmng/fpm/fpm_http_direct_request.c:126-127`: `"[pool %s] %s requires
pm = static"`). Is that restriction worth lifting for either process manager,
either direct executor (classic, `pool.executor = worker`), and is handing an
established connection to a peer process (strategy (c)) feasible in this
architecture at all?

## Verdict

**No cell is recommended. The restriction to `pm = static` stays, and is now a
dated decision rather than an unexplained rule.** Strategy (c) — handing a
live connection off to a peer process — is infeasible in this architecture for
the reasons in its own section below, not merely unmeasured.

| pm ∖ executor | classic | `pool.executor = worker` |
| --- | --- | --- |
| `static` | control; not a candidate | control; not a candidate |
| `dynamic` | **rejected** — configuration gate is the status quo answer, and even where the gate is absent (nginx arm) the retirement path already fails rule 3 | **not measured, blocked by #64** for anything depending on the master's idle/active view; the shared handler path itself is exercised (see below) and would fail rule 3 identically, since retirement drops in-flight requests before the master's view enters into it |
| `ondemand` | **rejected**, same reason as `dynamic` | **not measured, blocked by #64**, same caveat as above |

The decision rule (`build/benchmark-http-direct-pm.md`) needs all four of its
criteria to hold. Applying it:

1. **Idle memory saving ≥ 50 MB and ≥ 30% of pool footprint** — **not
   measured for a direct pool**: the configuration gate means #169 has no
   `dynamic`/`ondemand` row for `executor ∈ {classic, worker}` to compare
   against `static` at all (0/128 of those cells ran; every one came back
   `refused`, #169). The nearest same-binary evidence is the nginx arm, which
   does allow all three process managers on the same script and workload: pool
   PSS total across `pm ∈ {static, dynamic, ondemand}` at matched idle count
   stays in a 6.0–8.6 MB band with no consistent direction (`static`/4 children
   plain, idle 0: 7.53 MB; `dynamic`/4 children, same: 7.07 MB —
   `findings-benchmark-artifacts-066/issue-169-measurement-run/results.json`).
   That is nowhere near the rule's 50 MB floor in either direction, on this
   binary's per-child footprint (~2.0–2.2 MB PSS plain, ~2.8–3.1 MB TLS idle-0,
   ~5.8–5.9 MB TLS holding 256 idle connections, same file, `executor ∈
   {classic, worker}`, `workload = steady`). Rule 1 cannot be cleared or
   failed here — the data needed to judge it doesn't exist for direct pools,
   and is written as "not measured" rather than inferred from the nginx figures,
   which are a different process (`build/benchmark-http-direct-pm.md`'s own
   nginx-arm caveat: total-system and behaviour reference only, not a
   per-worker comparison).
2. **Successful-only p99 regression ≤ 10% / ≤ 2ms** — **not measured**, same
   reason: no direct `dynamic`/`ondemand` row exists to regress against
   `static`.
3. **`n_conn_closed_without_response = 0` and `n_client_visible_failures = 0`
   at every scale-down** — **fails, today, on `pm = static` itself.** All 24
   direct retirement rows (classic and worker) with a request in flight at
   the signal lost it: `n_conn_closed_without_response ==
   inflight_requests_at_t0 == 4` in every one, no exceptions (#169; `results.json`
   `workload=retire`, `retired_pid` rows). This is the load-bearing fact for
   the whole verdict: rule 3 is not a question that `dynamic`/`ondemand` would
   introduce — the retirement path that both process managers would have to
   use already fails it under `pm = static`'s own periodic retirement
   (`pm.max_requests`). Lifting the gate would make a path that already drops
   in-flight requests fire routinely instead of only at `max_requests`
   rollover; that is a regression on its own, independent of memory. The
   nginx arm's own retirement is a no-op by construction (nginx keeps every
   client socket; `build/benchmark-http-direct-pm.md`'s nginx-arm section), so
   its 0/0 rows are the yardstick, not a counterexample: 0 client-visible
   failures out of the reference stack's requests, versus 92 total across the
   direct grid at `pm.max_requests = 200` (#169's outcome comment).

   **This is narrower than "no drain path exists", per #166's own
   re-investigation** — worth recording here because it changes what the
   follow-up work actually is. A working drain path already exists:
   `fpm_direct_retire_enter()` / `fpm_direct_retire_done()`
   (`sapi/fpmng/fpm/fpm_http_direct.c:230`, `:280`) stop accepting and wait for
   the live-connection count to reach zero, bounded by `http.read_timeout`.
   It is reached by **SIGUSR1**. But the master's own scale-down path
   (`fpm_pctl_kill_idle_child()`, `fpm_process_ctl.c:342-350`) sends
   **SIGQUIT**, which lands on a different gate — the top of
   `fpm_direct_tick_body()` (`fpm_http_direct.c:345`), which exits on
   `!w->pending` (responses in flight, not connections held) and then
   unconditionally drops whatever is left. That gate mismatch, not a missing
   mechanism, is what #165's 18/15/19 `n_conn_closed_without_response` figures
   and #169's 4/4-in-24 figures both measure — tracked now as its own issue,
   #310. Testing the drain path directly against SIGUSR1, bypassing the
   mismatch, confirms it works for idle connections held: deferral takes
   `n_conn_closed_without_response` from 9 to 0 with keep-alive connections
   idle at retirement (#166). It does **not** fully close rule 3 even then: a
   request in flight at the exact instant retirement begins can still be
   dropped, "statistically the same [failure rate] either way" at `idle = 0`
   — a distinct race, now tracked as #311. So the honest state of rule 3 is:
   one identified, well-understood mechanism (#310, likely a smaller fix than
   it looked before this investigation) plus one still-uncharacterised race
   (#311), both open. Neither is fixed today, so the verdict stands, but
   "stays broken forever" is not the right characterisation either.
4. **`t_replacement_accepting < t_last_conn_closed + 1000ms`** — **not
   checkable**: `t_last_conn_closed_ms` is null on every direct retire row in
   this run (see "not measured" cells below), because with
   `pm.max_requests = 200` the retiring child's connections are dropped by
   `fpm_http_direct_conns_free()` immediately rather than draining to EOF
   (`fpm_http_direct.c:1988-1989`: `fpm_http_direct_conns_free(w.conns);` then
   `evhttp_free(w.http);` at end of the worker loop, unconditionally). There is
   no "last connection closed" event to measure a bound against.

**Result: criterion 3 alone is a clean, checkable "no" — a scale-down that
drops in-flight requests fails the rule regardless of what criteria 1, 2, and 4
would have shown**, and criteria 1/2/4 are honestly "not measured" rather than
assumed to pass. No `sigkill_floor_hit` row exists in this run (`t_exit_ms`
min 7.4 / median 15.5 / max 112.4 across all 240 retire rows, all below the
1000ms `fpm_process_ctl.c:536` floor), so every one of these numbers is a real
drain decision, not a kill artefact.

## Strategy (c): hand connections off to a peer process

**Infeasible in this architecture, not merely unbuilt.** A retiring direct
child cannot give its established connections to a peer without losing the
state that makes them established connections:

- `SCM_RIGHTS` moves a file descriptor between processes, but the connection
  state that makes an accepted socket useful — libevent's `bufferevent` (parse
  state, buffered-but-unflushed output, the registered callbacks) — lives in
  the retiring process's heap, not in anything the kernel tracks alongside the
  fd. Handing off the fd without that state leaves the receiving process with
  a raw socket and no idea what request, if any, is mid-flight on it.
- Under TLS, the same connection additionally carries an OpenSSL `SSL` object
  — the negotiated cipher, sequence numbers, and any buffered TLS record
  fragments — which has no supported serialization for a live handshake or
  live session; there is no API to hand a mid-stream `SSL*` to a different
  process's `SSL_CTX` and have it continue.
- Even ignoring both of the above, a hand-off buys nothing new in plaintext
  that the kernel doesn't already do: workers on a direct pool share one
  inherited listening socket, so the kernel already distributes **new**
  connections across whichever children are still accepting (the same
  distribution problem #53 covers for the gateway). What hand-off would add is
  redistributing **already-open** connections away from a retiring child —
  and that is exactly the state that cannot be moved, per the two points above.

Since the state that would need to move is process-local and (under TLS)
partly unserializable, strategy (c) does not clear a lower bar than "measure
it and see" — it fails on the mechanism before a measurement could be designed.
Recorded here as infeasible, not as "not measured".

## What was measured, and where

- One run, one box (`piotr@192.168.8.50`), one binary (sha256
  `e9b6759489e8ede7b09906b5be8e36a38b16abbb6fd4494c6ff159f0d944120e`, PHP
  8.5.4 NTS, built 2026-09-02), 608 rows, three rotated rounds per cell with a
  1s settle before each 5s window.
- Grid: executor ∈ {classic, worker, nginx} × pm ∈ {static, dynamic, ondemand}
  × TLS ∈ {off, on} × idle ∈ {0, 256} × workload ∈ {steady, retire} × stream
  mode ∈ {keepalive, new-connection} × `pm.max_requests` ∈ {0, 200}.
- 480 `ok` rows, 128 `refused` rows — every refusal is the same one
  (`fpm_http_direct_request.c:126-127`), i.e. the configuration gate answering
  the spike's question by construction for the direct executors.
- Cells recorded as "not measured", with the reason (per #169's own
  bookkeeping, reproduced here because #170 cites it):
  - `pm ∈ {dynamic, ondemand}` on `executor ∈ {classic, worker}`: not
    measured, refused by the binary. 128 rows.
  - `t_last_response_byte_ms` on every direct retire row: not measured, the
    event never happened (no in-flight request got a last byte on a retiring
    connection).
  - `t_last_conn_closed_ms` on direct retire rows with `pm.max_requests = 200`:
    not measured, connections were dropped by `fpm_http_direct_conns_free()`
    rather than drained to EOF.
  - Anything depending on the master's idle/active view of `pool.executor =
    worker`: not measured, blocked by #64.

## Risks this verdict does not need to weigh (out of scope, not overlooked)

- Whether a *fixed* retirement path (one that drains in-flight requests before
  the socket closes) would then clear rules 1/2/4 is genuinely unknown — that
  is future work, gated on the retirement path itself changing, not on this
  spike's data.
- `--enable-fpmng-fiber`'s executor was not part of this grid; #66 never asked
  for it, and it has its own build gate and constraints (see
  `docs/spike-sleep-yield-report.md`).

## Follow-up issues

- **None opened for implementing `dynamic`/`ondemand`** — the recommendation
  is "no", so #66 correctly puts implementation out of scope rather than
  spawning a follow-up for it.
- **No throwaway branch from #66's own spike work is merged.** #165 (dynamic),
  #166 (deferred retirement, strategy (a) — its condition *did* fire and it
  produced a real result, see above; it was not closed as not-needed), and
  #167 (ondemand) all stay not-for-merge; #169's measurement run used a
  separate, explicitly logged throwaway binary (`ALERT: ... requires pm =
  static` on every refused row) rather than any of those branches directly.
- **#310** opened: the master's scale-down sends SIGQUIT to the stopping gate
  in `fpm_direct_tick_body()` instead of SIGUSR1 to the drain path in
  `fpm_direct_retire_enter()`/`fpm_direct_retire_done()` — the specific,
  narrower mechanism #166 identified behind rule 3's failure.
- **#311** opened: even under the correct drain gate, a request in flight at
  the exact instant retirement begins can still be dropped at `idle = 0` — a
  distinct race #166 found and explicitly flagged as "worth its own issue off
  #66's write-up", separate from #310.
- Neither #310 nor #311 is an implementation of `dynamic`/`ondemand` — both
  are prerequisites rule 3 needs regardless of which process manager eventually
  uses the retirement path, so opening them does not contradict the "no
  implementation issue" bullet above. If both land and rule 3 clears, #66's
  recommendation should be re-run against rules 1/2/4, which this report
  records as "not measured" rather than assumed.
