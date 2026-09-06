# 037 — `O_NONBLOCK` leaks onto the listening socket and survives a reload

**Priority:** high. A classic pool that never used the fiber executor is left in an
unbounded respawn storm. Measured, reproduced twice, with a runaway of 6,400+
forks in 20 seconds.
**Status:** open.

## Context

`sapi/fpmng/fpm/fpm_pool_coop.c:490-495` sets `O_NONBLOCK` on
`fpm_globals.listening_socket` once, at container start, in every child of a
fiber pool. The comment above it reasons carefully about siblings **within the
pool** and about the async pool, and concludes the flag is confined to them.

It is not. `O_NONBLOCK` is a property of the **open file description**, not of a
descriptor. The listening socket is created in the master before `fork()`, so the
child shares one description with the master — and setting the flag in the child
mutates what the master holds.

## What was measured (2026-09-06, on the shared test box)

Built from `main` at `054294d`. Two hypotheses were tested; one was refuted.

**Refuted — two pools on the same `listen`.** FPM rejects this during config
validation, before any socket work:

```
ERROR: [pool classic_pool] unable to set listen address as it's already used in another pool 'fiber_pool'
```

The cross-pool socket reuse in `sapi/fpm/fpm/fpm_sockets.c:276-289` is therefore
unreachable for this defect. Recorded here so nobody re-investigates it.

**Confirmed — reload via `exec`.** With a fiber pool running, the flag is already
visible in the **master's own** descriptor, before any reload:

```
/proc/<master>/fd/8 -> socket:[8928306]   flags: 04002      (04000 = O_NONBLOCK)
```

Changing that pool to `pool.type = fastcgi` and sending `SIGUSR2`, the master
re-execs and inherits the descriptor:

```
NOTICE: using inherited socket fd=8, "127.0.0.1:20602"
/proc/<master>/fd/8 -> socket:[8928306]   flags: 04002      (same inode, flag intact)
```

The classic children then use a blocking `accept()` with no preceding poll
(`main/fastcgi.c:1395`), and thirteen lines later `EAGAIN` matches neither
`EINTR` nor `ECONNABORTED`, so `fcgi_accept_request()` returns -1 and the child
exits (`main/fastcgi.c:1408-1412`):

```
NOTICE: [pool relb2] child 1498127 exited with code 0 after 0.002340 seconds from start
NOTICE: [pool relb2] child 1498128 exited with code 0 after 0.002451 seconds from start
```

With no `emergency_restart_threshold` set this ran 6,400+ respawn cycles in about
20 seconds before the master was killed. A second, time-boxed run reproduced the
same signature (681 respawns in ~1s).

**Negative control.** A classic pool on its own `listen`, never touched by a
fiber pool, shows `flags: 02000002` (no `O_NONBLOCK`) and zero respawns. The
defect is specific to descriptor sharing, not a general classic-pool problem.

## Why this matters more than it looks

The victim is a **classic** pool — main-line behaviour, not the fiber track. The
fiber executor is moving behind a build flag that is off by default, which
narrows who can reach this, but does not remove it: anyone who tries fiber once
and then reloads gets a fork storm on pools that never had anything to do with
it. The failure is also silent in the wrong direction — the config is valid, the
master starts, and the damage shows up only as children dying instantly.

## What the fix has to satisfy

The current design has a child mutating state that outlives it and is shared
with the master. Whatever replaces it must not.

The pool-type contract (`sapi/fpmng/fpm/fpm_pool_type.h`) says per-type behaviour
is a field, a callback, or data — never `if (type == ...)` and never
`strcmp(type->name, ...)`. A socket flag that differs per pool type is exactly
that kind of per-type property, and the master knows the type before it forks.
Whoever picks this up decides the mechanism; this task does not prescribe it.

## Acceptance criteria

1. Start a fiber pool, then inspect the **master's** descriptor for that
   listening socket. The state of `O_NONBLOCK` there is whatever the design
   chooses, but it must be a deliberate, documented choice — not a side effect of
   a child having run.
2. Start a fiber pool on `listen` X, change that pool to `pool.type = fastcgi`
   with the same `listen`, send `SIGUSR2`. After the reload the classic pool
   serves a request, and its children show **zero** unexpected exits over at
   least 60 seconds. Evidence: the master log and a request that returns 200.
3. The reverse order also holds: a classic pool reloaded into a fiber pool on the
   same `listen` serves requests, and the fiber pool does not reintroduce the
   accept freeze that `fpm_pool_coop.c:475-489` describes.
4. A fiber pool and a classic pool that were never related still behave as the
   negative control above: the untouched pool's socket flags are unchanged.
5. The comment at `fpm_pool_coop.c:475-489` is corrected. As written it states a
   confinement that measurement disproved, and it is the reason the defect was
   introduced by a fix for something else.

## Notes

- Found while auditing whether the fiber and async work breaks anything in the
  classic FastCGI and HTTP paths. The other three interception points from the
  same audit — the `sleep` family, `php_stream_stdio_ops.set_option`, and
  `PS(mod)` — are process-local, installed after fork in
  `fpm_pool_fiber_child_main()` (`fpm_pool_fiber.c:552-567`), and are reached
  only through the pool type's `child_main` callback, so they cannot escape a
  fiber child. This one is different because it touches the kernel, not the
  process.
- The `O_NONBLOCK`-once approach itself was a fix for a real accept race; see the
  comment it carries. Reverting to per-accept toggling is not an option — that
  was the earlier defect.
