# 068 — Spike: dynamic and ondemand process managers for HTTP-direct

Status: open
Type: spike (decision-support analysis and measurement, not a feature commitment)

## Why

Task 054 restricts direct pools to `pm = static`, enforced by validation. The
reason is structural: a direct worker holds accepted connections in its event
loop, so a worker that the master wants to scale down or idle may own live
connections — unlike FastCGI workers, which own nothing between requests.
Whether dynamic/ondemand are worth supporting depends on what happens to those
connections, and that is a design question to answer before any feature task.

## Questions to answer

1. What are the candidate strategies for a worker told to scale down:
   (a) refuse and defer retirement until its connection count drops,
   (b) close idle keep-alive connections immediately and finish in-flight,
   (c) hand connections to a peer (feasible at all in this architecture?).
2. What are the memory and latency implications at small scale (direct pools
   are cheap per worker — does dynamic even pay off, or is static with small
   counts simply better)?
3. How do the strategies interact with graceful drain (task 067), the
   connection policy (task 063), and the master's spawn/spare logic?

## Acceptance criteria

- A written analysis of the candidate strategies with a recommendation,
  grounded in measurements: worker RSS cost per idle keep-alive connection,
  retirement latency per strategy, and an nginx+FastCGI dynamic-pool baseline
  on the poligon for reference.
- If a strategy is recommended, a follow-up feature task is drafted with
  acceptance criteria; if none pays off, a negative result is recorded and the
  validation restriction is documented as permanent in `docs/http-direct.md`.
- Raw artifacts retained outside the tree.

## Out of scope

- Implementing dynamic/ondemand; the spike only decides whether to.
- Changes to `http`/`fastcgi` process managers.
