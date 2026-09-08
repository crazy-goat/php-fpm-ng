# 052 — Measure the per-request cost of a non-empty `fiber.isolate_statics` list

**Priority:** medium. The list grows with every framework version bump;
nobody has ever measured what an entry costs.
**Status:** open.

## What is missing

Task 008 measured (argued, really) only the **empty** list: both hooks start
with `if (count == 0) { return; }`, so an unconfigured pool pays nothing
(`sapi/fpmng/fpm/fpm_pool_coop_statics.c`). A latency measurement for the
empty-vs-4-entry case was attempted there and abandoned — curl
process-spawn noise dominated a 50-request loop; the zero-cost claim rests
on the code path, not a number.

The published Laravel list is now **six entries** (task 025) and will grow.
The claim that matters for adopters is not "empty is free" but "the
configuration we tell you to run costs X". Nothing measures X.

## What this task must produce

1. A measured per-request cost of the enter/leave swap for 0, 4 and 6
   entries, on a workload that does touch the isolated statics (Laravel
   bootstrap) and one that does not (Slim 4, which needs no entries). The
   measurement must not be curl-loop-noise-dominated: use a keep-alive
   client, many sequential requests, and report the spread, not just a mean.
2. A statement in `docs/frameworks.md` next to the versioned Laravel snippet:
   what six entries cost per request, so the list growing to ten entries one
   day is a decision informed by a number.
3. If the cost is measurable and non-trivial, the mechanism itself stays
   untouched — this task measures and documents; optimizing the swap would
   be a separate task.

## Explicitly out of scope

- Changing `fpm_pool_coop_statics.c`.
- Throughput or concurrency benchmarks (the Symfony 172 req/s measurement is
  a separate exercise on dedicated hardware).
