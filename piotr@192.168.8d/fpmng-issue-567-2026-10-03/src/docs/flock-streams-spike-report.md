# flock() via the streams layer, with an in-process registry: spike

Date: 2026-09-06. Branch `spike/flock-streams`, worktree
`/Users/piotr.halas/work/php-fpm-ng-worktrees/flock-streams`. Test box
`192.168.8.50`, working directory `~/rd/b1`, ports 18931-18933/18940-18941
(within the assigned 18931-18950 range). Not committed to `main`, nothing
pushed to any remote. `/Users/piotr.halas/work/php-src` was not modified —
the build overlays `sapi/fpmng/` onto a `cp -al` copy of php-src on the test
box, exactly as `docs/flock-fiber-deadlock-report.md` (spike/flock-fiber)
did.

## Verdict

**Feasible, with one important caveat found and fixed during the spike, and
one caveat that remains inherent to the approach.**

Intercepting `PHP_STREAM_OPTION_LOCKING` on the plain-files stream, with a
per-process in-process registry layered strictly on top of the real
`flock()` syscall, closes the E2 mechanism from the prior spike (userland
`flock()`/`file_put_contents(..., LOCK_EX)` deadlocking a `pool.executor =
fiber` worker when the lock holder suspends on a socket). Measured: 0/20
unrelated requests affected post-fix, versus 20/20 pre-fix. Mutual exclusion
is proven correct both within one process and across two processes under
real concurrent load. Overhead on the uncontended path is a few
microseconds.

**The caveat found during this spike, not anticipated going in:** a naive
two-phase implementation (check in-process conflict once, then loop
NB-probing the real kernel lock and eventually fall back to a genuinely
blocking `flock()` for "cross-process" contention) reintroduces the exact
same deadlock class it set out to fix, just one level up — because the
"final fallback: block on the real syscall" step does not re-check whether
someone *else in the same process* won the lock while this fiber was
yielding between probes. It can. When it does, the process ends up blocking
its own single OS thread waiting for a lock held by one of its own
(suspended, otherwise-fine) fibers, which can now never run again to release
it. This was caught live with `gdb -p` on the actually-stuck worker (see
"Bug found and fixed" below) and fixed by folding both phases into one loop
that re-checks the in-process registry on every iteration, including
immediately before the final blocking-call fallback. After the fix, the
same stress scenario that deadlocked (20 concurrent requests split across
two separate worker processes contending for one file) now completes
20/20, correctly, twice in a row.

**The caveat that remains, inherent, not a bug:** this does not touch
`ext/session`'s `mod_files.c`, which calls `flock(2)` directly on its own fd,
never through the streams API. E1 (the session case) is unaffected by this
work — see the prior spike's option 2/3 discussion for that.

## What was built

Two new files, `sapi/fpmng/fpm/fpm_pool_fiber_flock.{c,h}`, plus three
minimal hooks in the existing `fpm_pool_fiber.c` (per the project's
"existing files get minimal hooks only" rule):

- `fpm_pool_fiber_flock_install()` called once from
  `fpm_pool_fiber_child_main()`, right after `fpm_pool_fiber_xport_install()`
  (same timing rationale: after all extensions' MINIT). It saves
  `php_stream_stdio_ops.set_option` and replaces it with a wrapper — the
  exact same "wrap a mutable global `php_stream_ops` in place" trick the
  prior spike's read-only research identified as viable for the plain-files
  wrapper (`main/streams/php_stream_plain_wrapper.h:18`,
  `PHPAPI php_stream_ops php_stream_stdio_ops`, not `const`).
- `fpm_pool_fiber_flock_release_owner(void *owner)` called from
  `fpm_fiber_after_switch()` in `fpm_pool_fiber.c`, at BOTH places a
  request's fiber can end: the normal `ZEND_FIBER_STATUS_DEAD` path, and the
  "suspended outside the scheduler (Fiber::suspend() in the request's main
  fiber?); dropping it" abnormal path. `owner` is the same opaque handle
  `fpm_pool_fiber_waiter()` already hands out (no new identity concept
  needed) — reusing it means a nested user-level `Fiber` inside a request
  still resolves to the *request's* identity, which is what the registry
  needs.

Design invariant, and the property that made the fix in the middle of this
spike safe to reason about: **the in-process registry never replaces the
kernel-level `flock()` as the source of truth.** Every successful acquire,
on every code path including every fallback, still goes through a real
`flock()`/`flock(LOCK_NB)` call before this code considers itself holding
anything. If the registry is wrong, disabled (full), or buggy, every path
still asks the kernel directly — worst case is parking the process again
(the bug we found and fixed does exactly this), never two fibers or two
processes both believing they hold an exclusive lock at once. This was
checked explicitly (see "Cross-process correctness" below) — it holds.

Algorithm (`fpm_fiber_flock_set_option()`, single loop after the fix):

1. `LOCK_UN` always delegates to the real `flock(LOCK_UN)` first, then clears
   this owner from the registry entry (if any) and wakes any local waiters.
2. Otherwise, identify the file by `fstat(fd)` `st_dev`+`st_ino` (not the
   path — two paths can be the same file). If `fstat`/`php_stream_cast`
   fails, or the registry table is full (4096 entries cap), fall back to the
   *exact original* `php_stdiop_set_option` call unmodified — correct, just
   without the in-process shortcut.
3. Loop: while another fiber in *this* process holds a conflicting mode
   (`LOCK_EX` conflicts with anyone else; `LOCK_SH` only conflicts with an
   `LOCK_EX` holder), and the caller didn't ask for `LOCK_NB` itself, suspend
   on an in-process wait queue via the existing
   `fpm_pool_fiber_wait_wake(NULL)`/`fpm_pool_fiber_wake()` primitives — no
   polling, a real wait/wake pair, woken by the holder's `LOCK_UN`. If the
   caller passed `LOCK_NB`, return `EWOULDBLOCK` immediately instead of ever
   suspending or blocking (non-blocking semantics preserved exactly). If this
   fiber cannot suspend (nested user `Fiber`, GC destructor context — checked
   via the existing `fpm_pool_fiber_can_wait()`) or the wait queue is full
   (64 cap), fall through to a real blocking `flock()` right here instead —
   a documented, rare-case regression to today's behavior, not a new bug.
4. Once nobody in this process conflicts, try the real lock with
   `LOCK_NB` added. Success: register this owner as the holder, return.
   `EWOULDBLOCK`: this is cross-process contention (or someone in this
   process won it during our last yield — see the bug below) — either way,
   loop back to step 3, which will correctly detect a new in-process holder
   if one appeared, or try the real lock again after a short cooperative
   sleep (`fpm_pool_fiber_wait_wake(&interval)`, not the OS thread — the
   fiber yields, the scheduler runs everyone else) if not.
5. After a bounded number of such rounds (default 5, ~100ms of cooperative
   backoff; `FPMNG_FLOCK_POLL_ATTEMPTS` env var overrides for measurement,
   `0` disables polling entirely), give up and do one real *blocking*
   `flock()` — but only having just re-confirmed, in the same iteration,
   that nobody in this process holds it. This last call can still block the
   OS thread, bounded by whichever *other process* holds the kernel lock.

## Bug found and fixed during this spike

The first version of this file split steps 3 and 4/5 above into two
one-shot phases instead of one loop. Under real cross-process load (20
concurrent `counter.php` requests split 10/10 across two separate
`pool.executor = fiber` worker processes, `pm.max_children = 1` each,
contending for one file), both worker processes wedged permanently —
`ps`/`/proc/<pid>/stack` showed both stuck in `locks_lock_inode_wait` for
minutes, no self-recovery, only `kill -9` (by pid, on my own processes)
recovered them.

Root-caused live with `sudo gdb -p <pid> -batch -ex bt`: the stuck frame was
this file's own final-fallback real blocking `flock()` call
(`fpm_flock_real` at the bottom of `fpm_fiber_flock_set_option`), not
anything upstream. Printing the registry entry at that frame
(`entry->ex_owner`) showed it set to a *different fiber in the same
process* — confirmed independently by `ss -tnp`, which showed a CLOSE-WAIT
socket (data already delivered, unread) owned by that same pid, to the slow
test server the holder was suspended on. Mechanism: the old two-phase code
checked the in-process registry once, found it empty, and moved to
cross-process NB-probing; between two probes it cooperatively yielded
(`wait_wake(&interval)`), and *during that yield* a different fiber in the
same process won the real lock and correctly suspended on its own socket
wait. The first fiber, resuming from its probe sleep, saw `EWOULDBLOCK`
again but — because phase 2 never re-checked the in-process registry — kept
treating it as pure cross-process contention, exhausted its retry budget,
and made a genuinely blocking `flock()` call. That call can only be
satisfied by the very fiber it just froze the OS thread out from under.
Permanent, self-inflicted, same-process deadlock — the same bug class the
whole file exists to remove, reintroduced through the one code path that
didn't re-check its own registry.

Fix: merged into one loop (see algorithm above) that re-checks the
in-process conflict at the top of every iteration, including the one
immediately before the final blocking fallback, so a same-process winner
that appeared during a cooperative yield is always caught before ever
touching a real blocking syscall.

Re-ran the identical 10+10 cross-process stress scenario after the fix,
twice: both times, 20/20 responses, final counter value 20, every value
1..20 present exactly once, both worker processes healthy and responsive
immediately after (`ping.php` answered normally). See "Correctness" below
for the full numbers.

## Measurements

All against the fixed build. Two separate `pool.executor = fiber` worker
processes (`fiber1` on `127.0.0.1:18931`, `fiber2` on `127.0.0.1:18933`,
each `pm.max_children = 1`) plus one `classic` control pool (no
`pool.executor`, `127.0.0.1:18932`, `pm.max_children = 4`, so the streams
hook is never installed there — used as the "unpatched" baseline for
overhead, see below).

### E2 repro: no longer deadlocks

`flock_a.php` (opens the file, `flock(LOCK_EX)`, `fsockopen()` to a slow TCP
server with a 5s delay, writes, `LOCK_UN`) and `flock_b.php` (opens the same
file, `flock(LOCK_EX)`, `LOCK_UN`), same shape as the prior spike's harness,
copied from `~/rd/a9/test/` (read-only reference, not modified) plus new
scripts of my own (`counter.php`, `uncontended.php`, `lock_then_die.php`) in
`~/rd/b1/test/docroot/`.

```
A pid=1080016 connect_ms=0.2 socket_total_ms=5001.3 unlock_ms=0.2 total_ms=5001.8
B pid=1080016 flock_wait_ms=4003.7 total_ms=4003.7
```

B, started 1s after A, waits ~4000ms (exactly the remainder of A's ~5s hold)
and then completes normally — the same "B waits its turn" shape the prior
report's E3 classic-pool control showed, now reproduced on the **fiber**
pool with a single worker process. Confirmed at the syscall level: `ps` and
`sudo cat /proc/<pid>/stack` on the worker mid-wait showed `ep_poll`
(libevent scheduler idle, waiting for events), not `locks_lock_inode_wait`
— B never touched the blocking syscall at all.

### Blast radius: 20/20 (was 20/20 dead)

20 concurrent, unrelated `ping.php` requests fired at the same worker during
the A/B contention window above:

```
20 200
```

All 20 returned HTTP 200. Prior spike, same scenario, no fix: 20/20 timed
out with no response.

### Correctness: mutual exclusion holds under real interleaving (same process)

`counter.php`: `flock(LOCK_EX)`, then `fsockopen()` to a slow server (0.15s
delay, forces a real fiber suspend *while holding the lock*), then a
read-increment-write of a counter stored in the same file, then `LOCK_UN`.
20 concurrent requests against the single-worker `fiber1` pool:

```
final counter file contents: 20
values seen: 1,2,3,...,20 — each exactly once, no duplicates, no gaps
lock_wait_ms: 0.1, 152.1, 301.6, 452.7, ... (steps of ~150ms, 20 steps)
```

The ~150ms step in `lock_wait_ms` is direct evidence of real serialization —
every request but the first genuinely waited out the previous holder's full
hold (which itself included a real suspend on a socket) before proceeding.
If the registry had ever let two fibers both believe they held `LOCK_EX`,
this would have shown as a value skipped or repeated in the file; it never
did, across the run reported here and a second confirmation run.

### Cross-process: kernel-level exclusion still holds, not bypassed

Direct A/B, two **different processes** (`fiber1` and `fiber2`), same file:

```
A (process fiber1, pid 1080016): socket_total_ms=5001.0 unlock_ms=0.3 total_ms=5004.3
B (process fiber2, pid 1080020, started 1s later): flock_wait_ms=3999.6 total_ms=4002.5
```

Same ~4s wait shape as the same-process case — confirms the in-process
registry does not shortcut or bypass the real kernel lock for a different
process; B genuinely waited on A's real `flock()` hold.

Split-load correctness, 10 `counter.php` requests to `fiber1` concurrently
with 10 to `fiber2`, same shared counter file (this is the scenario that
deadlocked before the fix described above; **after the fix**, run twice):

```
run 1: 20/20 responses, final counter = 20, values 1..20 each exactly once
run 2 (immediately after, same processes, no restart): 20/20, values 1..20 each exactly once
```

Both worker processes answered `ping.php` normally immediately after each
run — no lingering damage, no restart needed.

### Leaked-lock-on-death cleanup: verified

`lock_then_die.php`: opens the file, `flock(LOCK_EX)`, then calls an
undefined function to force an uncaught fatal error — no `LOCK_UN`, ever.
Immediately afterward, a plain `flock_b.php` request for the *same file*:

```
B pid=1080016 flock_wait_ms=0 total_ms=0.015s
```

Zero wait — the registry entry was released when the fatal-erroring
fiber's request ended (`fpm_fiber_after_switch()`'s normal-completion path,
which runs regardless of whether the script died, since
`fpm_coop_req_run()` already wraps script execution in `zend_try`/
`zend_catch`). This is the "a fiber that dies while holding a lock" case
from the task description; it does not leak.

Not separately exercised: the *other* release call site, in the
"suspended outside the scheduler (`Fiber::suspend()` in the request's main
fiber?)" branch — this requires the request's own top-level fiber (not a
nested user `Fiber`) to suspend some way other than through this project's
scheduler, which is already a documented rare/abnormal case upstream of
this work. The hook is there and structurally identical to the verified
one; it was not separately load-tested.

### Overhead on the uncontended path

`uncontended.php`: 2000 back-to-back `fopen`+`LOCK_EX`+`write`+`LOCK_UN`
cycles on a fresh file, no contention, no suspension. Compared: `fiber1`
(hook installed) vs `classic` (hook never installed there — the install
call only happens in `fpm_pool_fiber_child_main()`, so this is a true
apples-to-apples "same php-fpm-ng binary, same file shape, hook vs no
hook" comparison, not a different build):

```
classic (no hook), 3 runs: 10.38, 10.20, 10.36, 10.17 us/op
fiber (hook installed), 3 runs (after warmup): 14.64, 14.17, 14.53 us/op
```

Roughly **+4 to +4.5us per lock+unlock pair** (~40% relative, single-digit
microseconds absolute) — consistent with the design: one extra `fstat()`
call per `flock()`/`LOCK_UN` call (for dev+inode identity) plus a linear
scan over a small in-memory array (registry lookup + conflict check, O(1)
in practice since one file rarely has more than a handful of concurrent
holders). No dynamic allocation on this path — the registry is a static
array, sized in the design for a modest number of concurrently-locked files
per process (4096 entries, 64 waiters/sh-holders per entry).

## What this does NOT cover

- `ext/session`'s `mod_files.c` raw `flock(2)` call — unaffected, out of
  scope, see the prior spike's report for that (E1).
- Any other blocking syscall not routed through `php_stream_lock()`
  (`sleep()`, `curl`, `pdo_pgsql`, ...) — unaffected, same as before this
  spike and the previous one.
- A nested user-level `Fiber::suspend()`/GC-destructor context reaching
  `flock()` while genuinely conflicting in-process: falls back to a real
  blocking `flock()` immediately (cannot suspend the request's own
  scheduler fiber from inside a nested context) — a real, but narrow and
  pre-existing-class, regression path, not a new failure mode introduced by
  this design.
- The registry is per-process, static-sized (4096 files, 64
  waiters/sh-holders per file); exceeding either cap degrades to the exact
  original (pre-hook) behavior for the overflowing case rather than
  misbehaving — checked explicitly in the code, not separately load-tested
  to actually hit either cap.
- `LOCK_SH` (shared/read locks) coexistence and `LOCK_EX`/`LOCK_SH`
  upgrade/downgrade by the same owner are implemented and reasoned through
  in the registry logic, but not separately measured under concurrent
  load in this spike (only `LOCK_EX` was stress-tested, since that is what
  both `flock()`-with-writes and `file_put_contents(..., LOCK_EX)` use in
  practice).

## What would still need doing for production

- A real regression suite exercising `LOCK_SH`, `LOCK_NB` explicitly passed
  by the caller (verified structurally: returns `EWOULDBLOCK` without ever
  suspending, but not stress-tested under contention), and the two
  documented-but-not-load-tested overflow caps.
- Deciding the registry's static sizing properly (4096 files / 64
  waiters-or-readers per file were spike defaults, not derived from any
  real workload).
- The `FPMNG_FLOCK_POLL_ATTEMPTS` / poll-interval knobs are read once via
  `getenv()` at first use and cached for the life of the process — fine for
  a spike, would want a proper `pool.fiber.*`-style ini directive (matching
  how `pool.executor = fiber` config already works) for production, with
  validation in `fpm_coop_validate()`.
- A harder look at whether the "nested `Fiber`/GC-destructor can't suspend,
  fall through to real blocking flock()" path can be tightened (e.g., can a
  GC destructor holding a stream ever legitimately need to flock() at all?)
  rather than accepted as-is.
- Everything already flagged in the prior spike's report as unaffected by
  this work (`ext/session`, other blocking syscalls) stays exactly as
  flagged there.

## Files

New, in this worktree:
- `sapi/fpmng/fpm/fpm_pool_fiber_flock.c`
- `sapi/fpmng/fpm/fpm_pool_fiber_flock.h`
- `docs/flock-streams-spike-report.md` (this file)

Minimal hooks in existing files:
- `sapi/fpmng/fpm/fpm_pool_fiber.c`: `#include "fpm_pool_fiber_flock.h"`;
  one call to `fpm_pool_fiber_flock_install()` in
  `fpm_pool_fiber_child_main()`; two calls to
  `fpm_pool_fiber_flock_release_owner(fr)` in `fpm_fiber_after_switch()`
  (normal-completion path and the "dropped" abnormal path).

Test harness, on the test box only (`~/rd/b1/`, not copied into the
worktree — throwaway, per the same convention the prior spike used):
`~/rd/b1/test/php-fpm-fiber1.conf`, `php-fpm-fiber2.conf`,
`php-fpm-classic.conf`, `~/rd/b1/test/docroot/{flock_a,flock_b,ping,
counter,uncontended,lock_then_die}.php` (`flock_a.php`, `flock_b.php`,
`ping.php` copied unmodified from `~/rd/a9/test/docroot/`, treated
read-only; the other three are new for this spike), `~/rd/b1/test/
slow_server.py` (copied unmodified from `~/rd/a9/test/`),
`~/rd/b1/cross_counter_test.sh` (driver for the cross-process split-load
test).

## Cleanup performed after this report was written

- Stopped all three `php-fpm-ng` masters (`fiber1`, `fiber2`, `classic`) by
  their pid files under `~/rd/b1/test/run/`, not `pkill php-fpm`.
- Stopped both `slow_server.py` instances (ports 18940, 18941) by pid.
- Left `~/rd/b1/` in place (build tree, install, test harness) in case this
  needs to be re-run or extended; nothing outside `~/rd/b1/` and ports
  18931-18933/18940-18941 (within the assigned 18931-18950 range) was
  touched, except read-only reads of `~/rd/a9/test/` to copy its harness
  scripts.

## Fix applied after this spike: never block on a confirmed in-process conflict

Date: 2026-09-06, same branch, follow-up commit. Found by re-reading this
report's own "What this does NOT cover" section against the
`tests/fiber-blocking-red` suite (item 11): the "cannot suspend / waiter
queue full" fallback in `fpm_fiber_flock_set_option()`'s conflict loop fell
through to a real *blocking* `flock()` right after confirming, in the same
iteration, that a fiber in this same process holds the conflicting lock.
That holder cannot run again until the calling fiber's blocking `flock()`
returns — the process has exactly one OS thread — so that call can never
be satisfied. This is not "a documented rare-case regression to today's
behavior for this one call" as the original comment claimed; it is the
exact permanent whole-process deadlock this file exists to remove, reached
from a different edge (queue-full or can't-suspend) than the two-phase-loop
bug found and fixed earlier in this same spike (see above). Every request
in flight in that worker dies with it.

**Fix:** both trigger conditions (fiber cannot suspend per
`fpm_pool_fiber_can_wait()`; 64-slot waiter queue full) now fail the lock
attempt immediately — `errno = EWOULDBLOCK; return -1;` — instead of
calling `fpm_flock_real()`. This is exactly the failure shape a real
non-blocking `flock()` already produces elsewhere in this same function
(the `nb` branch), and exactly what `ext/standard/file.c` already turns
into ordinary, documented userland behavior: `php_flock_common()`
(`flock($fp, LOCK_EX, $wouldblock)`) returns `false` and, when `$wouldblock`
was passed by reference, sets it to `true` on `EWOULDBLOCK`;
`file_put_contents($f, $d, LOCK_EX)` returns `false` with an `E_WARNING`
when `php_stream_lock()` fails. Verified against php-src
(`ext/standard/file.c:179-204` `php_flock_common`, `:472,490`
`php_stream_lock` call in `PHP_FUNCTION(file_put_contents)`) before
deciding this, not assumed. No in-process holder is ever registered on this
path (the early `return -1` skips `fpm_flock_register_holder()` entirely),
so mutual exclusion is unaffected — a failed attempt never becomes a
phantom holder.

Decisions made alongside the fix, and the reasoning:

- **The two 64-slot caps (waiter queue, `LOCK_SH` holder list) are left as
  they are.** Growing either only raises the threshold at which the same
  problem resurfaces — a fixed-size registry can always be overrun by
  enough concurrent contention, and this spike's whole point is that
  *overrunning it must fail cleanly, not block*, so the cap's exact size is
  no longer safety-critical, only a performance/memory knob. The registry is
  a **static** array (`FPM_FLOCK_MAX_ENTRIES` = 4096 files per process,
  each carrying its own 64-slot waiter array and 64-slot `sh_owners` array
  inline, `struct fpm_flock_entry_s`), so raising either 64 multiplies by
  4096 entries per worker process regardless of how many files are actually
  contended in practice (e.g. 64 -> 256 waiters would add roughly
  4096 * (256-64) * 2 * sizeof(void*) ~= 12.6 MB of always-resident memory
  per worker, for a slot that is empty in the overwhelmingly common case).
  Keeping the cap and failing cleanly on overflow gets the same safety
  outcome as growing it, at zero extra static memory cost. If real
  workloads are later measured to hit 64 waiters on one file under
  *ordinary* load (not the 70-waiter stress test built specifically to
  exceed it), that would be a reason to revisit the number — not yet
  observed.
- **A fiber that genuinely cannot suspend (a nested user `Fiber`, a
  destructor running under GC) fails immediately, same as the queue-full
  case, not handled specially.** There is no third option: it cannot
  suspend on the in-process wait queue by construction (that queue is
  serviced by the very fiber scheduler such a context is already outside
  of), and blocking is exactly the deadlock this fix removes. Immediate
  failure is the only safe outcome available to it.
- **The cross-process polling defaults (5 attempts x 20 ms, then one real
  blocking `flock()`) are UNCHANGED and UNMEASURED beyond what this spike
  already reported.** They remain defensible in a way the in-process case
  never was: by the time this code reaches that final blocking call, it has
  just re-confirmed (same loop iteration, per the earlier two-phase-loop
  fix) that nobody in this process holds the conflicting lock, so the
  blocking call can only be satisfied by a *different, running* process —
  bounded by that process's own hold time, not by a suspended fiber that
  can never resume. But the specific numbers (5 x 20ms = up to ~100ms of
  cooperative backoff before the blocking fallback) were carried over
  as-is from the original spike defaults and were not re-derived or
  re-measured for this fix — stated plainly rather than left implicit.

See the task's suite run (`tests/fiber-blocking-red`, item 11 and items
5a/5b/6/7) for the acceptance measurements against this fix.
