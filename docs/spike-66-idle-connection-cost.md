# Spike #66: what an idle keep-alive connection costs a direct worker

Measurement task [#164](https://github.com/crazy-goat/php-fpm-ng/issues/164).
Measured 2026-09-12 on the poligon with
[`build/benchmark-http-direct-pm.py`](../build/benchmark-http-direct-pm.py) and
the decision rule in
[`build/benchmark-http-direct-pm.md`](../build/benchmark-http-direct-pm.md).

This document reports numbers. The verdict per (pm x executor) cell belongs to
[#170](https://github.com/crazy-goat/php-fpm-ng/issues/170); what is below is
the input to it.

All memory figures are mebibytes and kibibytes (2^20, 2^10) -- the units
`/proc/*/smaps_rollup` reports in.

## Run identity

| | |
|---|---|
| binary | `~/issue256-bin/php-fpm-ng`, sha256 `5eaa809db777a284f3d925ba71646b3d1631a25e7a35e2847f0d8d5e670a2da7` |
| what that binary is | the v0.2.0 build, i.e. with [#256](https://github.com/crazy-goat/php-fpm-ng/issues/256) fixed |
| version | `PHP 8.5.4 (fpm-fcgi) (built: Sep 2 2026 14:42:18) (NTS)` |
| kernel | `Linux lenovo 7.0.0-31-generic x86_64` |
| artifacts | `~/issue164-hold`, `~/issue164-drop`, `~/issue164-mc{1,2,4,8}` on the poligon |
| `results.json` sha256 | hold `6db26a2211905d83…`, drop `8322ae2463a06329…`, mc1 `9e902eecd753b9de…`, mc2 `8663b11974b6ead9…`, mc4 `2baefc3204e44bb1…`, mc8 `e794c2a854a33488…` |
| load average (1 min) | hold 0.13 -> 3.54, drop 3.54 -> 3.72, mc sweep 3.72 -> 5.43 |

The box is shared, and the `pm.max_children` sweep ran last, on a box already
carrying a load average between 3.7 and 5.4. It is also the noisiest data in
this document -- see the range on its per-connection figure below.

Settings: `pm = static`, `pm.max_children = 4` unless the table says otherwise,
`rlimit_files = 4096` stated in the pool config, an 8-connection request stream,
3 rounds of 5 s per cell after a 10 s settle, and N idle keep-alive connections
that are one request old and then silent. Medians over the three rounds.

**The pool is restarted for every round.** The first version of this matrix was
measured on a harness that did not, and its rounds were a memory-growth curve
of one long-lived pool rather than three samples of the same thing: the same
child pids appeared in rounds 1, 2 and 3 in 40 of 40 cells and pool Pss rose
monotonically across them in 40 of 40. Every number in the first draft of this
document was measured that way and none of them are carried over here. The fix
is [#163](https://github.com/crazy-goat/php-fpm-ng/issues/163),
`build/benchmark-http-direct-pm.py`.

## Why the memory column is Pss and not RSS

VmRSS counts libphp's text and every mapped extension once per child. Summed
over four children it reports 37.05 MiB where the pool's Pss is 7.77 MiB --
4.8x -- and the difference is pages that are never duplicated and never
returned when a child dies. Any "saving" computed from summed VmRSS is mostly
that artefact. Both are recorded; only Pss is summed.

## What one idle keep-alive connection costs

Pool Pss (all children), median of three rounds, against N held connections:

| executor | transport | N=0 | N=64 | N=256 | N=1024 | per connection |
|---|---|---|---|---|---|---|
| classic | plaintext | 7.75 | 7.89 | 8.31 | 10.02 | **~2.3 KiB** |
| classic | TLS | 10.61 | 13.89 | 23.83 | 63.65 | **~53 KiB** |
| worker | plaintext | 7.75 | 7.86 | 8.22 | 9.67 | **~1.9 KiB** |
| worker | TLS | 10.60 | 13.74 | 23.70 (*) | 62.42 | **~52 KiB** |

(*) The one Pss cell not to lean on. Its three rounds were 23.907 / 23.702 /
22.248 MiB, a 7.0 % spread, where the other fifteen cells of this matrix stayed
within 1.4 %.

The per-connection column is the slope at N = 1024. For classic it is the same
slope at all three points, so it extrapolates: 2272 / 2288 / 2320 B at
N = 64 / 256 / 1024 plaintext, 53 744 / 54 184 / 54 317 B on TLS. For worker it
is not flat -- 1760 / 1912 / 1970 B plaintext, rising 12 % across the range, and
51 376 / 53 660 / 53 066 B on TLS, which is not even monotone. Extrapolating a
worker cell beyond N = 1024 from one of these figures is a guess; the classic
rows are the ones that behave.

**A TLS connection costs 23x a plaintext one on classic and 27x on worker**
-- an `SSL` object with its record buffers, against a bare libevent
bufferevent. At N = 1024 that is 53 MiB
of Pss on top of a pool whose entire base footprint is 10.6 MiB. It is the only
large number in this document.

## File descriptors

One descriptor per connection, to within the couple the sample catches in
flight: classic plaintext goes from 52 descriptors at N = 0 to 1077 at
N = 1024, a delta of 1025, and classic TLS 53 -> 1076, a delta of 1023. The worker executor's base is
76 rather than 52 -- the notify streams #64 is about -- and it moves by the same
1024. What the aggregate hides is where they land.

| executor | transport | idle connections in the busiest child at N=1024 | share of 1024 |
|---|---|---|---|
| classic | plaintext | 362 | 35 % |
| classic | TLS | 584 | 57 % |
| worker | plaintext | 413 | 40 % |
| worker | TLS | 440 | 43 % |

These are `idle_conns_per_child`, not descriptor counts: every child also holds
13-21 descriptors that have nothing to do with N.

An even split over four children would be 256. The busiest child took 584.
Nothing balances accepted connections across children -- whichever child wins
the accept keeps the connection for its lifetime -- so a pool sized by
`total / pm.max_children` is sized for a distribution it does not get.

This is why the harness now states `rlimit_files` in the pool config (4096
here) instead of inheriting it: at the default 1024 the TLS cell above would
have hit the per-child limit somewhere between N = 1700 and N = 2200 aggregate,
and the failure would have been read as a connection-handling bug rather than
as this. It is a band and not a number because the busiest child's share is not
stable: for classic TLS it was 50 % at N = 64, 46 % at N = 256 and 57 % at
N = 1024, and the three rounds at N = 1024 split as 584 / 464 / 618. The
paragraph above is the reason -- a distribution nothing balances cannot be
extrapolated from one point.

**The descriptor limit binds before memory does, on plaintext.** 1024 plaintext
connections cost 2.3 MiB and 1025 descriptors.

## Dropping an idle connection returns the descriptor, not the memory

The same matrix twice: `http.read_timeout = 120 s`, which holds every idle
connection for the whole round, and `http.read_timeout = 5 s`, which drops them
during the 10 s settle. Pool Pss, MiB, median of three rounds:

| cell | held | dropped | descriptors held | descriptors after the drop |
|---|---|---|---|---|
| classic plaintext N=1024 | 10.02 | 10.03 | 1077 | 53 |
| classic TLS N=1024 | 63.65 | 63.64 | 1076 | 52 |
| worker plaintext N=1024 | 9.67 | 9.67 | 1100 | 76 |
| worker TLS N=1024 | 62.42 | 63.48 | 1100 | 76 |

The descriptors come back in full. The memory does not come back at all: all
sixteen dropped cells are within 1.7 % of their held counterpart, and the 1.7 %
is the worker-TLS row, which is above the held value, not below it.

So a burst of idle connections sizes the worker permanently. `pm.max_requests`
is the only thing in the process manager that undoes it, by replacing the
child. Why the allocator keeps the arena rather than returning it is not
measured here -- nothing in the direct path calls `malloc_trim(3)`, which is
the obvious candidate, but that is a reading of the source and not a
measurement.

## Latency

Successful-only percentiles of the 8-connection request stream, milliseconds,
median of three rounds:

| cell | N=0 | N=64 | N=256 | N=1024 |
|---|---|---|---|---|
| classic plaintext | 0.364 / 0.758 | 0.364 / 0.761 | 0.363 / 0.761 | 0.365 / 0.760 |
| classic TLS | 0.613 / 1.300 | 0.611 / 1.317 | 0.613 / 1.321 | 0.613 / 1.305 |
| worker plaintext | 0.309 / 0.850 | 0.310 / 0.846 | 0.309 / 0.832 | 0.315 / 0.852 |
| worker TLS | 0.690 / 1.703 (*) | 0.514 / 1.267 | 0.514 / 1.279 | 0.526 / 1.295 |

Held idle connections cost no measurable latency anywhere. p50 and p99 both
stand still from N = 0 to N = 1024.

(*) is the cell not to read. Its three rounds delivered 31 776, 52 444 and
70 396 requests, a 74 % spread, where every other cell in the hold matrix stayed
within 3.9 %. The median landed in the middle of a distribution that has nothing
in the middle. Its neighbours at N = 64, 256 and 1024 agree with each other to
3 %, so the row to compare against is those, not this one. The same cell in the
drop matrix behaves the same way (70 838 / 52 926 / 41 576, 55 %), which is the
only reason to believe it is the cell and not the moment. The cause was not
chased; the poligon is a shared box.

Throughput is noisier than memory throughout the worker rows -- the drop
matrix's worker-plaintext cells reach 5.5-6.9 % between rounds. Nothing in this
document rests on a throughput figure, and the latency percentiles above are
per-request, not per-round, so they are unaffected.

`n_client_visible_failures` is 0 in all 96 rows of both matrices, and no row
failed or was refused.

## The decision variable: pool footprint against `pm.max_children`

#66 asks what `pm = dynamic(min..N)` would save against `pm = static(N)` at a
fixed load. Today's binary refuses `pm = dynamic` for a direct pool
(`fpm_http_direct_request.c:105`), so that pair cannot be measured -- what
follows is the counterfactual, not a measurement of dynamic. A dynamic pool
scaled down to k children holds what a static pool of k children holds, so
`pm.max_children` was swept at one fixed load (classic, plaintext,
8-connection stream, N = 0 and N = 256):

| `pm.max_children` | Pss | VmRSS | p50 | p99 | requests in 5 s |
|---|---|---|---|---|---|
| 1 | 4.33 MiB | 9.29 MiB | 0.793 ms | 1.078 ms | 50 360 |
| 2 | 5.91 MiB | 18.49 MiB | 0.442 ms | 0.802 ms | 83 692 |
| 4 | 7.77 MiB | 37.05 MiB | 0.364 ms | 0.762 ms | 101 270 |
| 8 | 9.65 MiB | 68.94 MiB | 0.365 ms | 0.872 ms | 97 589 |

**The marginal Pss of one direct worker is 0.47-1.58 MiB**, falling as the pool
grows (1->2: 1.58, 2->4: 0.93, 4->8: 0.47 MiB per child). VmRSS suggests ~9 MiB
per child; almost all of that is shared.

Scaling a four-child pool down to one therefore returns about 3.4 MiB.
Decision rule 1 asks for >= 50 MiB and >= 30 %. On this box a direct pool would
need dozens of children before a scale-down could clear that bar on worker
footprint alone -- and `docs/http-direct.md:17` uses `pm.max_children = 4` in
its own example.

The sweep also looks at whether spreading connections thinly costs more: 256
idle connections cost 2448 / 2552 / 2288 / 4220 B each at 1 / 2 / 4 / 8
children. **The 8-child figure is not resolved by three rounds.** Its N=0 cell
came out at 9.390 / 9.646 / 9.960 MiB, a 5.9 % spread against 1.1 % for every
other cell in the sweep, and propagating that gives a per-connection cost
anywhere in 2288-5752 B -- whose lower bound is exactly the 4-child median of
2288 B (itself a tight 2112-2480). So this data cannot tell "8 children cost
twice as much per connection" from "8 children cost the same", and no
explanation of the difference is offered here because there is no established
difference to explain.

The one case where the numbers are large is TLS, and a scale-down does not
reach it: the memory belongs to a child that is holding connections, and
killing that child takes the connections with it (see #256 and
[#165](https://github.com/crazy-goat/php-fpm-ng/issues/165)).

## Not measured

- `pm = dynamic` and `pm = ondemand` themselves: refused by today's binary, so
  the table above is a counterfactual built from `static(k)`.
  [#165](https://github.com/crazy-goat/php-fpm-ng/issues/165) and
  [#167](https://github.com/crazy-goat/php-fpm-ng/issues/167) measure them
  behind a throwaway patch.
- Whether the retained memory can be given back. `malloc_trim(3)` is the
  obvious lever and nothing calls it; whether it would actually return these
  arenas was not tested.
- Anything depending on the master's idle/active view of a worker-executor
  pool: that executor never leaves ACCEPTING
  (`fpm_http_direct_worker.c:1622`), which is
  [#64](https://github.com/crazy-goat/php-fpm-ng/issues/64). The memory and
  descriptor numbers above do not depend on it.
- Time-to-capacity when load rises. It needs the accept-driven load shape
  (`--stream-mode new-connection`) against a pool that can actually grow, so it
  belongs to #165/#167, not here.
- Any connection count above 1024, and any request workload other than
  `steady`.
