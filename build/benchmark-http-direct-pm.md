# `benchmark-http-direct-pm.py` — process-manager harness for http-direct pools

Written for issue #163, which is the first task of spike #66 ("dynamic and
ondemand process managers for HTTP-direct"). Every measuring task in that spike
(#164–#168) and the measurement run (#169) use this harness.

## The decision rule

**Written before the first measurement, on purpose.** #66 ends in a
recommendation, and a recommendation whose bar is chosen after the numbers are
in can be argued either way. These are the numbers the recommendation is judged
against; if the spike wants to move them, it moves them in a commit with a
reason, not in the write-up.

`pm = dynamic` (or `ondemand`) for an http-direct pool is **worth having** only
if all four hold at a setting an operator would plausibly run:

1. **Idle memory saving ≥ 50 MB and ≥ 30 % of the pool's idle footprint**,
   measured as the sum of **`pss_child_bytes`** over the pool at the same
   `pm.max_children` and the same idle-connection count N, dynamic versus
   static. Judged on Pss, not VmRSS: VmRSS counts libphp's text and every mapped
   extension once per child, so summing it credits a retired child with tens of
   MB of shared pages that were never duplicated and are not returned when it
   dies — a phantom saving large enough on its own to clear the 50 MB bar this
   rule exists to enforce. `rss_child_bytes` is recorded next to it because it
   is the number an operator sees in `top`, and is quoted as such, never summed.
   *Why 50 MB:* a saving smaller than one worker's resident set is available
   today by setting `pm.max_children` one lower. That costs nothing, needs no
   new code, and is what the recommendation has to beat. The 30 % relative
   term is there so the rule still bites on a box where a worker is 300 MB.

2. **Successful-only p99 regression ≤ 10 %, or ≤ 2 ms absolute, whichever is
   larger**, against `pm = static` at the same N and workload. Successful-only:
   a merged percentile improves when requests start failing fast, which is the
   opposite of the property being bought. The absolute term exists because a
   p99 of 1.2 ms is noise-dominated and a 10 % rule on it measures the box, not
   the pool.

3. **`n_conn_closed_without_response` = 0 and `n_client_visible_failures` = 0**
   during every scale-down in the run. Not "low" — zero. A scale-down that
   drops a request is a correctness result, and a non-zero value here *is* the
   spike's answer: the retirement path has to be fixed before a process manager
   that uses it can be recommended at all.
   Connections closed while genuinely idle are counted separately
   (`n_conn_closed_while_idle`) and are **not** a failure by this rule — today's
   code drops them by design: at the end of the worker loop
   `fpm_http_direct_conns_free()` and then `evhttp_free()`
   (`fpm_http_direct.c:1988-1989`) free every tracked connection without waiting
   for the idle ones, and
   whether that is acceptable is a question for #66, not a threshold here.

4. **`t_replacement_accepting` < `t_last_conn_closed` + 1000 ms**, so the pool
   is not below capacity for longer than one maintenance pass. This one is a
   guard rail rather than a target: a replacement that takes seconds to accept
   turns a routine scale-down into a capacity dip.

Any run whose `t_exit_ms` is at or past `sigkill_floor_ms` (1000 ms,
`fpm_process_ctl.c:536`) is flagged by the harness and must not be quoted as a
drain time. Under a master-driven scale-down that number means SIGKILL:
`fpm_pctl_kill_idle_child()` escalates on the maintenance pass after the one
that sent SIGQUIT (`fpm_process_ctl.c:342-350`).

If 1–4 do not all hold, the spike's answer is "no", and the negative result is
the deliverable.

## What it measures

One row per (executor, pm, TLS, N, workload, round) in `results.json`:

| field | meaning |
| --- | --- |
| `rss_child_bytes` | VmRSS per child pid, sampled with N idle connections established |
| `pss_child_bytes` | Pss per child (`smaps_rollup`); the only one of the two that may be summed across a pool |
| `idle_conns_per_child` | how the idle set actually landed across the children |
| `steady` | successful-only and non-2xx latency, split, plus window failures |
| `retirement.t_exit_ms` | child gone from `/proc`, from t0 |
| `retirement.t_last_response_byte_ms` | last byte of the last reply the retiring child sent |
| `retirement.t_last_conn_closed_ms` | last connection it held going to EOF |
| `retirement.t_replacement_accepting_ms` | first reply from a pid the pool did not have at t0 |
| `retirement.n_conn_closed_without_response` | requests in flight when the socket died |
| `retirement.n_client_visible_failures` | 5xx, resets and timeouts in the retirement window only |
| `retirement.sigkill_floor_hit` | `t_exit_ms >= 1000`; the row measured a kill, not a drain |

t0 for every `t_*` is one timestamp: the harness's own, taken immediately before
it sends SIGQUIT to the chosen child. The signal is sent by the harness because
the master's SIGQUIT has no timestamp observable from outside; it is the same
signal on the same child-side path. The master's SIGKILL escalation is **not**
reproduced this way — see the flag above.

## Usage

```
build/benchmark-http-direct-pm.py <php-fpm-ng> <scratch-dir> \
    --executors classic worker --pms static --tls-modes plain tls \
    --idle 0 1024 --workloads steady retire --rounds 3
```

The binary must carry the http-direct marker and its sha256 is recorded in
`metadata.json`; pointing the harness at a stock `php-fpm` makes it refuse
rather than produce numbers for a different program.

`--stream-mode new-connection` drives the load by accepts instead of by requests
on connections that are already open. It matters for any scale-up question: all
children accept on the same inherited listening socket, so a child spawned now
receives no share of the connections already established (#53), and a "time to
capacity" number taken under keep-alive would show dynamic as useless by
construction.

`--idle 1024` needs 1024 descriptors on the client side too; the harness raises
its own `RLIMIT_NOFILE` soft limit towards the hard limit and refuses to start
if the hard limit is too low, rather than quietly measuring fewer connections.

## Shared-box rules

The poligon is shared. The harness uses its own scratch directory and stops
each pool through the pid file it wrote itself, escalating to that pool's own
process group if the master wedges. Nothing is ever matched by binary name.

Its whole port range is bound up front and **held**: each port is released only
in the moment its own pool starts (`SO_REUSEADDR`, so there is no window). A
bind-and-close check would only cover t=0, and the last scenario of a twelve-row
run starts minutes later — a port taken in between would come back as "pool
never listened", which reads like a broken binary.

## Known limits

- `pm = dynamic` and `pm = ondemand` are refused at startup by today's binary
  for an http-direct pool (`fpm_http_direct_request.c:105`). The harness records
  that refusal as a row with `result: "refused"` and the log lines that caused
  it, which is the honest state of the dimension until #66 lands a change.
- The response parser handles only what this harness's own front controller
  emits. A chunked response raises instead of being decoded: from this pool it
  would be a finding, not a format to support.
- The idle set is established by one request per connection, sequentially. At
  N = 1024 that is a second or so of setup, and which child accepts each
  connection is the kernel's choice, not the harness's — `idle_conns_per_child`
  reports the distribution that actually happened.
