# 050 — Transient Redis reply misattribution under the configured isolation list

**Priority:** medium. Seen once in a passing run; silent-wrong-data class.
**Status:** open.

## Where it was seen

2026-09-08, task 025 run on the test box (192.168.8.50), a *configured*
Laravel pool (six-entry `fiber.isolate_statics`, `pm.max_children = 1`,
phpredis 6.3.0RC1), logged at 05:45:25:

    JsonException: Syntax error ... json_decode('+OK')  (routes/web.php, a Redis GET)

A `+OK` status reply parsed as the payload of a `GET` means a reply meant for
a different call was consumed by this one — two requests sharing one phpredis
connection or one corrupted reply queue. The run's scenarios all passed
anyway, so it self-healed and was observed exactly once. It is recorded in
`findings.md` (2026-09-08, task 025) and was confirmed substantive by review.

## Why it matters

The reflection audit (task 025) verifies class statics only. phpredis
connection state is not a class static, so an audit `COVERED` verdict cannot
see this path. It is the same silent-wrong-data class as the statics leaks,
one level lower.

## What this task must produce

1. A reproduction (a dedicated probe route with several concurrent
   `SET`/`GET` round-trips on phpredis, run repeatedly — the Laravel `/mix`
   scenario already does this; a loop around it may be enough).
2. A root cause: which shared structure routed two requests onto one
   phpredis connection (candidate: a static or service outside the isolated
   set caching `\Redis` objects — e.g. anything resolving through a
   non-isolated path), or a genuine phpredis/fpm-ng interleaving bug.
3. Either an isolation-list entry, a C fix, or a documented limitation.

## Explicitly out of scope

- predis (a pure-PHP client has no extension-level state).
