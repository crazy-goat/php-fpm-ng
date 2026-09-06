# In-process session-lock arbiter for `session.save_handler = files` under `pool.executor = fiber`: spike

Date: 2026-09-06. Branch `spike/session-lock-arb`, worktree
`/Users/piotr.halas/work/php-fpm-ng-worktrees/session-lock`. Base: `main`
(`65ec755`, "async: reject executor until hardened"). Prior work: the
deadlock itself was proven in `docs/flock-fiber-deadlock-report.md` on
branch `spike/flock-fiber` (worktree
`/Users/piotr.halas/work/php-fpm-ng-worktrees/flock-spike`), which
recommended, as a stopgap, rejecting `session.save_handler = files` under
`pool.executor = fiber`. This spike investigates the harder "layer on top,
without patching php-src" option that report explicitly deferred ("build it
as a real prototype and measure it").

**Status: MEASURED (both variants).** All six measurements requested for
Variant 1 (`files_arb`) have been run on a real build on the test box and
pass — see "## Measurements" below. One real bug (a use-after-free causing a
reliable SIGSEGV on the first contended in-process lock handoff) was found
while measuring and fixed without changing locking/ordering semantics — see
"## Bug found and fixed during measurement". **Variant 2**
(`fpm_pool_coop_session_patch.c`, protects plain `session.save_handler =
files` with no pool reconfiguration) has also now been built and measured —
see "## Measurements — Variant 2" near the bottom. All eight measurements
requested for it pass, but two things surfaced during that pass that revise
the design section below: (1) a build-time bug (a Zend-core macro absent
from this box's php-src snapshot) and a real crash-causing bug (swapping the
module out from under an already-open `auto_start`-opened session corrupts
memory) were found and fixed; (2) the "a second `session_start()` in the
same request is protected" claim needed a correction — it is only true when
a fiber suspend/resume happens between the close and the reopen, not
unconditionally as originally written.

## Verdict (mechanism confirmed by source reading AND measured on a real build)

**Feasible, but NOT as a transparent override of `session.save_handler =
files`.** The mechanism the earlier report's design sketch hoped for
("re-register a module named `files`, it wins") **does not work** — verified
by reading `ext/session/session.c` directly (see "Mechanism" below). The
working mechanism requires:

1. A **new, distinct save-handler name** (`files_arb` in this prototype),
   registered via the real `php_session_register_module()` PHPAPI call.
2. An **explicit ini override**, `session.save_handler = files_arb`, in any
   pool that wants the protection. There is no way to make plain
   `session.save_handler = files` pick up the wrapper — this is not an
   implementation shortcut, it is forced by how `ext/session`'s own request
   startup resolves the handler (see below). This is a real, load-bearing
   finding: **the fix is opt-in per pool config, not automatic.**
3. Calling `php_session_register_module()` (and, once, `_php_find_ps_module("files")`
   to capture the original implementation to delegate to) is a **real link-time
   dependency on `ext/session`'s exported symbols** — unlike every other hook
   this project has added around `ext/session` so far (`fpm_pool_coop_session.c`
   goes out of its way to avoid exactly this, see its own header comment), this
   feature cannot avoid it. This is a new, honestly-reported maintenance/build
   risk: if `ext/session` is ever built `--enable-session=shared` without also
   ensuring this object is linked after it (or if the symbols are ever renamed/
   removed upstream), the whole SAPI binary fails to link. The project has never
   used `--enable-session=shared` so far (every build command in prior reports
   uses it built-in), so this is a latent risk, not an active break.

## Mechanism — what actually overrides the handler, and why the "same name" idea fails

Read directly from `/Users/piotr.halas/work/php-src` (read-only, unmodified),
commit checked out there at the time of reading (working tree matches the
`spike/flock-fiber` report's PHP-8.5-dev checkout unless noted).

- `ext/session/session.c:1208-1210`:
  ```c
  static const ps_module *ps_modules[MAX_MODULES + 1] = {
      ps_files_ptr,
      ps_user_ptr
  };
  ```
  `ps_files_ptr` (`&ps_mod_files`, the built-in `files` handler) is **hard-coded
  at index 0**, forever, for the life of the process. `php_session_register_module()`
  (`session.c:1213-1224`) only ever *appends* into the first free `NULL` slot at
  index ≥ `PREDEFINED_MODULES` (2) — it can never occupy index 0.
- `ext/session/session.c:1488-1499` (`_php_find_ps_module`):
  ```c
  for (module_index = 0, current_module = ps_modules; module_index < MAX_MODULES; module_index++, current_module++) {
      if (*current_module && !strcasecmp(name, (*current_module)->s_name)) {
          found_module = *current_module;
          break;
      }
  }
  ```
  A **linear scan from index 0**, first name match wins. Registering a second
  module also named `"files"` can never be found — the original at index 0
  always matches first. **This settles the first open question: re-registering
  the same name does NOT override it; a different name is structurally
  required.**
- `ext/session/session.c:2815-2833` (`php_rinit_session`, called as the real
  `PHP_RINIT_FUNCTION(session)` on **every** request, including inside this
  project's one-shot coop container's own `php_request_startup()`):
  ```c
  PS(mod) = NULL;
  {
      const char *value = zend_ini_string_literal("session.save_handler");
      if (value) {
          PS(mod) = _php_find_ps_module(value);
      }
  }
  ...
  if (auto_start) {
      php_session_start();
  }
  ```
  This is the second load-bearing finding, **not assumed by the earlier
  report**: `PS(mod)` is unconditionally reset to `NULL` and **re-resolved
  from the current `session.save_handler` ini string, every single request**
  — it is not "resolved once at MINIT and left alone" the way `session.save_path`
  is (which is why `fpm_pool_coop_session.c`'s `mh_arg2` trick works for that
  directive but is not sufic for this one). There is no window in which we can
  install our wrapper into a live `ps_globals.mod` pointer and have it survive
  into the next request — `php_rinit_session()` throws that value away and
  looks the name up fresh, every time, via the exact table described above.
  Consequently **the only lever available is the ini string plus the contents
  of `ps_modules[]`** — which is exactly the "differently-named module plus an
  ini default" branch the task asked us to check, not the "does re-registering
  win" branch. Also note: **if `session.auto_start = 1`, the auto-start
  `session_start()` call happens synchronously *inside* `php_rinit_session()`**,
  i.e. before any per-request hook of ours could run after RINIT returns — this
  is why the wrapper must win at the ini-string/table level, not via a
  post-RINIT patch.
- `ext/session/mod_files.h:18-19`: `extern const ps_module ps_mod_files;
  #define ps_files_ptr &ps_mod_files` — exported, but referencing it directly
  is exactly the class of link dependency this project's `fpm_pool_coop_session.c`
  already avoids for `ps_globals`; we instead fetch the original module via
  `_php_find_ps_module("files")` once, at container start — same *kind* of
  dependency (a real, exported `ext/session` symbol) just at one remove, and it
  does not remove the fundamental risk noted above.

## Design of the prototype

New files, `sapi/fpmng/fpm/fpm_pool_coop_session_lock.[ch]` (not yet
committed at the time of writing this section — see file list at the bottom
for what actually landed). Four lines added to the existing
`sapi/fpmng/fpm/fpm_pool_coop.c` (this project's own file, not php-src — the
"minimal hooks, new file for new behavior" rule applies to php-src, and this
project already hooks its own `fpm_pool_coop.c` this way for the `ini`,
`statics` and `session` isolation features; this is the fourth):
1. In `fpm_coop_container_start()`, before `php_request_startup()`: install
   the arbiter (register `files_arb`, capture the real `files` module).
2. In `fpm_coop_req_enter()`: record "the ctx currently on the CPU" (needed
   because the save-handler callback signature carries no request identity —
   we attribute a lock to whichever ctx is running when it is acquired).
3. In `fpm_coop_req_free()`: the leak safety net — force-release any
   in-process lock still attributed to a ctx being torn down, same call
   pattern as the existing `fpm_coop_ini_req_free` / `fpm_coop_statics_req_free`.

Per-session-id lock table: a process-global `HashTable` keyed by the session
id `zend_string` content (hash+memcmp, not pointer identity — deliberately,
since the exact `zend_string*` object backing a given id is not guaranteed
stable across `s_read`/`s_close` calls). Each entry: `held` flag, owning ctx
pointer, and a FIFO queue of `fpm_pool_fiber_waiter()` handles. A second
HashTable indexes entries by owning-ctx-pointer for O(1) force-release.

Locking is only inserted around `s_read` (per the task's own framing — this
is also where `mod_files.c`'s first `ps_files_open()`/`flock()` happens in
the normal `session_start()` path: `session.c:480`,
`PS(mod)->s_read(&PS(mod_data), PS(id), &val, ...)`). Release happens in
`s_close`, **after** delegating to the real `files` close (which is what
actually `close()`s the fd and drops the kernel `flock`) — so a woken waiter
in this process always finds the kernel lock already free, never merely
"about to be freed". `s_write`, `s_destroy`, `s_gc`, `s_create_sid`,
`s_validate_sid`, `s_update_timestamp` are plain pass-through wrappers
(no locking) — they reach `mod_files.c` only after `s_open`/`s_read` already
serialized this process's fibers for that id.

This design directly answers the "is the session id reliably available"
open question, and answers it more strongly than the earlier design sketch
assumed: **we do not need to parse cookies, guess `session_id()` calls, or
special-case `session_regenerate_id()`/a second `session_start()` in the same
request at all** — `PS_READ_ARGS` hands `s_read` the exact `zend_string *key`
that `ext/session` itself resolved through *its own* logic (cookie, URL,
`session_id()`, regenerated id, whatever), for every call, on every path,
because we sit at the same choke point `mod_files.c` itself uses. This
closes the biggest caveat the earlier (unbuilt) design sketch flagged.

## What is NOT covered / known limitations (stated up front, honestly)

- **Opt-in only.** Nothing protects a pool that leaves
  `session.save_handler = files` as-is; the fix requires editing pool config
  to `files_arb`. A misconfigured or forgotten pool gets zero protection and
  no warning at request time (only a one-time NOTICE-level log line at
  container start saying the arbiter is installed and what name to use).
- **New link dependency** on `php_session_register_module()` /
  `_php_find_ps_module()`, discussed above.
- **Not real FIFO fairness** across waiters: release uses Mesa-style
  signal-one-and-recheck, so a freshly-arriving `s_read` for the same id can
  overtake a waiter that has been queued longer. Does not affect mutual
  exclusion correctness, only ordering under heavy contention.
- **A user script running its own nested `Fiber` that calls
  `session_start()` from inside it** is a case `fpm_pool_fiber_can_wait()` may
  not support suspending through; the wrapper detects this (logs once) and
  falls back to calling the real handler with no in-process lock — i.e. the
  original deadlock risk still exists in that specific, narrow corner case.
- **Kernel-level flock is untouched and still layered underneath** — see the
  cross-process measurement below for why this matters and what was checked.
- **Table memory is unbounded only by distinct session ids ever seen while
  contended**; entries are deleted once unheld with no waiters, so steady
  state is bounded, but this was not stress-tested for cardinality.

## Variant 2 — patching `ps_globals.mod` in place (protects plain `files`, no reconfiguration)

Raised as a pushback from a second reviewing agent: the `files_arb` variant
above requires pool reconfiguration and a real link dependency on
`php_session_register_module()`/`_php_find_ps_module()`. Is there a route
that protects plain `session.save_handler = files` with zero reconfiguration
and no new link-time dependency? Investigated and built as a second,
parallel prototype: `sapi/fpmng/fpm/fpm_pool_coop_session_patch.[ch]`, hooked
into `fpm_pool_coop.c` alongside (not instead of) the `files_arb` variant —
both compile into the same binary and are inert for pools that don't match
their respective condition, so they do not conflict.

**Yes, mostly — with one structural gap (`session.auto_start`) the other
variant does not have.**

### Mechanism

`ps_globals.mod` (`const ps_module *mod;`, `php_session.h:138`) is an
ordinary struct field — `const` qualifies what it points AT, not the field
itself, so the pointer is freely writable. `fpm_pool_coop_session.c` already
gets `ps_globals`'s address with no link dependency, via the `mh_arg2` of the
`session.save_path` ini entry; `fpm_pool_coop_session_patch.c` duplicates
that same trick independently (its own copy, so it has no compile-time
dependency on that file either) and, after letting the real per-request
RINIT resolve `PS(mod)` as usual, simply overwrites the field:

```c
ps->mod = &psw_mod;   /* psw_mod: our wrapper, s_name copied from the real "files" module */
```

**Identifying the built-in `files` module without a linker symbol:** capture
happens in `fpm_coop_session_patch_container_start()`, called before this
process's first-ever `php_request_startup()`. At that point in a process's
life, `ps_globals.mod` can only hold whatever `OnUpdateSaveHandler` wrote at
ini-registration time for the configured default — verified that RINIT is
the *only* thing that ever resets it to `NULL`/re-resolves it
(`session.c:2815-2833`), and RINIT hasn't run yet. So if the pool's default
`session.save_handler` is `"files"`, whatever `ps->mod` already contains at
this exact moment is *guaranteed* to be the real, built-in `ps_mod_files` —
nothing else could have been substituted in without a RINIT or an
`ini_set()` happening first, and neither has. The `s_name == "files"` string
check in the code is belt-and-suspenders, not the actual identification
mechanism — the mechanism is the **timing** (before-first-RINIT). Per
request thereafter, `fpm_coop_session_patch_req_apply()`/`_req_enter()`
re-derive the same decision RINIT itself just made, by reading the current
`session.save_handler` ini string (`zend_ini_string_literal`, a Zend-core
call, not an `ext/session` symbol) and comparing to `"files"` — if it
matches, `_php_find_ps_module()` (called by RINIT moments earlier) is
guaranteed to have returned the one true built-in module, so there is
nothing left to misidentify.

**The `auto_start` gap (real, not fixed by this variant):**
`php_rinit_session()` calls `php_session_start()` *itself*, synchronously,
*inside* RINIT, when `session.auto_start = 1` — before
`fpm_coop_session_request_startup()` returns, i.e. before this variant's
post-RINIT patch ever runs. **The first `session_start()` call of a request
with `auto_start = 1` is NOT protected by this variant** — it runs through
the real, unwrapped `files` module. Any subsequent `session_start()` in the
same request (after a `session_write_close()`) IS protected, since the patch
has run by then. The `files_arb` variant does not have this gap: it wins
RINIT's own `_php_find_ps_module()` lookup directly, so even the
auto-started `session_start()` inside RINIT already uses the wrapper. This
is the single biggest correctness trade-off between the two variants.

**`ini_set('session.save_handler', ...)` mid-request:** `OnUpdateSaveHandler`
(`session.c:582-611`) writes `ps_globals.mod` directly whenever
`session.save_handler` is `ini_set()` at runtime, invisibly to this file. If
a script switches away from `"files"` and back within one request, the patch
is silently gone with no trace — rejected as an acceptable outcome (a
silent loss of protection is worse than a validated bypass). Chosen fix:
**detect and self-heal**, not "refuse" or "warn-and-stay-broken".
`fpm_coop_session_patch_req_enter()` runs the same check on *every* fiber
resume (the coop scheduler's own natural checkpoint, not just the start of a
request) — so within one scheduler tick of the `ini_set()`, the wrapper is
silently reapplied UNLESS it's the very first time this happens in the
process, in which case it logs once at WARNING (see log line below), making
it visible in practice, not just self-healing in theory.

### What is NOT covered (in addition to everything the `files_arb` variant already shares — same lock table, same Mesa-style release, same nested-user-Fiber fallback, same per-process-only guarantee)

- **The `auto_start` gap above** — structural, not a bug, not fixed here. **Measured
  and refined below**: the gap is real, but it is WIDER than the prose above
  originally claimed. See "## Measurements — Variant 2", point 7: a second
  `session_start()` in the same request is protected only if a fiber
  suspend/resume happens between closing the first session and opening the
  second — a bare `session_write_close(); session_start();` with no
  intervening I/O is **not** protected, and reproduces the full kernel-level
  deadlock. The original wording above ("Any subsequent `session_start()` in
  the same request... IS protected") is corrected by that measurement; kept
  here unedited otherwise so the discrepancy is visible, not silently fixed.
- **Field-overwrite, not a documented extension API.** `ps_globals.mod` is
  not exposed for this purpose; a future `ext/session` refactor that changes
  its layout, its type, or moves the RINIT/write-timing described above
  would silently break this variant with no compile error (unlike a removed
  PHPAPI symbol, which at least fails to link).
- **Name-based-at-a-specific-moment identification** is sound as argued
  above, but is more subtle to audit than "did `_php_find_ps_module()` find
  our registered name" — a future maintainer re-reading this code without
  the timing argument in front of them could plausibly "fix" the ordering
  and silently break the safety property.



While running measurement #2 below (E1 against the fixed pool) the worker
process **segfaulted** (SIGSEGV) on the very first run, reliably, every
time it was retried. This was a real, load-bearing bug in the prototype,
not a measurement artifact — found and fixed as part of this pass, per the
task's instructions to fix minimally and report what changed, without
altering locking semantics.

**Symptom**: `[pool f] child <pid> exited on signal 11 (SIGSEGV - core
dumped)`, master respawned a fresh worker immediately. First seen at
13:04:37 (275s uptime, likely from an earlier smoke-test request), then
reproduced deterministically on demand by re-running the E1 shape with a
fresh session id (`arbtest2`).

**Diagnosis**: reproduced with core dumps enabled (`ulimit -c unlimited`,
`core_pattern` pointed at `~/rd/b2/test/log/` for the duration of this one
capture, then restored to the box's original apport pattern immediately
after — the box's `core_pattern` is a systemwide sysctl, so this was done
for the shortest possible window and reverted). `gdb -batch -ex 'bt full'`
on the resulting core:

```
#0  arb_waiter_enqueue (e=<optimized out>, handle=0x7c983b8026e0) at fpm_pool_coop_session_lock.c:135
        135         e->waiters_tail->next = w;
#1  arb_lock_acquire (key=...) at fpm_pool_coop_session_lock.c:209
#2  arb_read (mod_data=..., key=..., val=..., maxlifetime=1440) at fpm_pool_coop_session_lock.c:290
#3  php_session_initialize () at ext/session/session.c:494
#4  php_session_start () at ext/session/session.c:1739
#5  zif_session_start (...) at ext/session/session.c:2704
...
#11 fpm_coop_execute (ctx=...) at fpm_pool_coop.c:626   [executing session_b.php's source, confirmed from the embedded file_handle buffer in the frame]
```

Disassembly of the live-optimized code around the fault (`disassemble
arb_read`, cross-referenced against `fpm_coop_session_lock_table`'s live
state in the core — `nNumOfElements = 0`, i.e. genuinely empty at the time
of the fault) showed the crash was a **use-after-free**, not a first-touch
null/garbage pointer: B's fiber, after being woken from
`fpm_pool_fiber_wait_wake()`, resumed holding a stale `arb_entry *e` whose
memory had already been `efree()`'d and reused for something else while B
was suspended.

**Root cause, in `arb_lock_release()`** (before the fix):

```c
e->held = false;
arb_clear_owner(e);
arb_wake_one(e);                 /* dequeues the one waiter and marks it runnable */

if (!e->waiters_head) {          /* true immediately after dequeuing the only waiter! */
    zend_hash_del(&fpm_coop_session_lock_table, key);   /* efree()s e right here */
}
```

`arb_wake_one()` only **marks** a waiter's fiber runnable
(`fpm_pool_fiber_wake()`) — it does not run it synchronously. The woken
fiber does not actually resume until the scheduler gets back to it, later
in the same process's event loop (in this measurement: after A's own
request finishes its RSHUTDOWN/response teardown, which runs first and
performs its own allocations). By the time B's fiber resumes, it re-enters
`arb_lock_acquire()`'s `while (e->held)` loop and dereferences the *same*
`e` again — but `e->waiters_head` was empty right after B itself was
dequeued (there was only ever one waiter), so `arb_lock_release()` had
already deleted and freed it. The freed 32 bytes were reused for an
unrelated allocation in the interim, so B read garbage out of where
`e->waiters_tail` used to be and crashed trying to link a new waiter node
behind it (`e->waiters_tail->next = w`, with `e->waiters_tail` now garbage).

**Fix applied** (`sapi/fpmng/fpm/fpm_pool_coop_session_lock.c`, in this
worktree, committed to the working tree — not upstream php-src): only
delete the entry when release did **not** just hand off to a waiter.
`arb_wake_one()` now returns `bool` (whether it actually woke someone), and
`arb_lock_release()`'s deletion is guarded by that:

```c
if (!arb_wake_one(e) && !e->waiters_head) {
    zend_hash_del(&fpm_coop_session_lock_table, key);
}
```

This does **not** change locking or ordering semantics at all — mutual
exclusion, the Mesa-style signal-and-recheck design, and wake order are
untouched. It only changes *when* the bookkeeping struct's memory is
reclaimed: never while a just-woken waiter might still touch it. After the
fix, the entry stays alive until either the woken waiter re-acquires it
(setting `held = true` again, keeping it referenced) or a later release
finds no waiters at all left to hand off to.

Rebuilt and reran the exact repro that crashed before (`arbtest3`) three
times after the fix: no crash, no core dump, correct output each time (see
measurement #2 below). Also reran the N=30 correctness test (measurement
#5) and the cross-process test (measurement #6) against the fixed binary —
both pass. This bug would have hit **every single contended in-process
session lock handoff** (i.e. exactly the scenario the whole feature exists
for), so it was on the critical path, not an edge case — good that
measurement caught it before this went anywhere near being recommended.

## Measurements

Binary: `/home/piotr/rd/b2/inst/sbin/php-fpm-ng`, built from this worktree
(after the bugfix above) via `build/prepare.sh ~/rd/b2/phpsrc` +
`./buildconf --force` + the same `configure`/`make` recipe as
`docs/flock-fiber-deadlock-report.md`. Test box `192.168.8.50`, working
directory `~/rd/b2` (own directory, own ports 18951-18970, per the task's
constraints).

### 1. Binary identity check

```
$ strings ~/rd/b2/inst/sbin/php-fpm-ng | grep -F 'fpm_pool_coop_session_lock.c'
/home/piotr/rd/b2/phpsrc/sapi/fpmng/fpm/fpm_pool_coop_session_lock.c
fpm_pool_coop_session_lock.c
$ strings ~/rd/b2/inst/sbin/php-fpm-ng | grep files_arb
files_arb
[pool %s] coop-session-lock: php_session_register_module('files_arb') failed (module table full?) - in-process session-lock arbiter NOT installed
[pool %s] coop-session-lock: in-process session-lock arbiter installed as save handler 'files_arb' (delegates to 'files'); set session.save_handler = files_arb to use it - plain session.save_handler = files is NOT overridden and gets no protection from this feature
```

**Verdict: PASS** — the binary embeds our new source file's path and its
log strings; it is our build, not a stale/reused one.

### 2. E1 repro re-run, now fixed (Pool F, `session.save_handler = files_arb`)

Pool F: `pool.executor = fiber`, `pm.max_children = 1`, port 18951,
`session.save_path` its own dir. Slow server on port 18960 (5s delay).

```
COOKIE="PHPSESSID=arbtest3"
curl -s -m 30 -b "$COOKIE" http://127.0.0.1:18951/session_a.php?slow_port=18960 &   # A
sleep 1
curl -s -m 15 -b "$COOKIE" http://127.0.0.1:18951/session_b.php &                    # B
```

Output:
```
=== A ===
A pid=1104968 connect_ms=0.1 socket_total_ms=5000.8 session_close_ms=0.1 total_ms=5001 data='slow-ok'
real  0m5.013s
=== B ===
B pid=1104968 session_start_wait_ms=3993.8 total_ms=3993.8 hits=2
real  0m4.007s
```

Both A and B completed with **real HTTP responses**, same worker pid
(1104968) throughout — no worker restart, no crash. B waited ~3.99s
in-process (arriving at the ~1s mark, A released at ~5s: matches the
expected remaining hold time) then proceeded and correctly saw `hits=2`
(A's increment plus its own).

Worker state captured mid-hold (separate run, same shape, cookie
`arbtest4`), 2s into A's 5s hold, with both curls still in flight:

```
$ sudo ps -o pid,ppid,stat,wchan:32,etimes,cmd -p 1104968
    PID    PPID STAT WCHAN                            ELAPSED CMD
1104968 1104966 S    ep_poll                               30 php-fpm: pool f
$ sudo cat /proc/1104968/stack
[<0>] ep_poll+0x496/0x4c0
[<0>] do_epoll_wait+0x58/0xd0
[<0>] __x64_sys_epoll_wait+0x6c/0x130
...
```

The worker is parked in `ep_poll` (its libevent scheduler loop), **not**
`locks_lock_inode_wait`/`__do_sys_flock` — confirms B is suspended via our
in-process waiter, not blocked in the kernel, and the scheduler is free to
run (which is what let A's socket read complete and release the lock).

**Verdict: PASS** — no deadlock, real responses, correct shared counter,
worker never blocked in the kernel.

### 3. Same E1 shape against the unfixed control (Pool F-unfixed, `session.save_handler = files`)

Pool F-unfixed: identical to Pool F except `session.save_handler = files`
(plain), port 18952. Slow server on port 18961.

```
COOKIE="PHPSESSID=unfixedtest1"
curl -s -m 30 -b "$COOKIE" http://127.0.0.1:18952/session_a.php?slow_port=18961 &
sleep 1
curl -s -m 15 -b "$COOKIE" http://127.0.0.1:18952/session_b.php &
```

Output: **both curls hit their client timeout with zero bytes of
response** (`real 0m30.011s` for A, `real 0m15.014s` for B — identical
shape to the original flock-fiber report). Worker state 3s in:

```
$ sudo ps -o pid,ppid,stat,wchan:32,etimes,cmd -p 1104974
    PID    PPID STAT WCHAN                            ELAPSED CMD
1104974 1104972 S    locks_lock_inode_wait                 44 php-fpm: pool funfixed
$ sudo cat /proc/1104974/stack
[<0>] locks_lock_inode_wait+0xb4/0x1f0
[<0>] __do_sys_flock+0x1db/0x220
[<0>] __x64_sys_flock+0x14/0x20
...
```

Worker recovered only via `kill -9` (same as the original report); master
respawned a fresh child immediately after.

**Verdict: PASS (as a control)** — this build reproduces the exact
original deadlock when `session.save_handler` is left at plain `files`,
confirming (a) the deadlock mechanism is unchanged on this build/binary,
and (b) it is specifically `files_arb` that makes the difference in
measurement #2, not something incidental about the rebuild.

### 4. Blast radius with the fix (Pool F)

During a fresh A/B hold (cookie `blastfix1`, A on port 18960's slow
server), fired 20 concurrent `ping.php` requests (`curl -m 5`) at Pool F
starting ~1.5s in (both A and B still in flight):

```
ping 200s: 20/20
```

All 20 returned HTTP 200. A and B both completed correctly afterward
(`A: total_ms=5001.4`, `B: session_start_wait_ms=4001.3 hits=2`).

**Verdict: PASS** — blast radius is **0/20** with the fix, versus the
pre-fix baseline's measured **20/20** (from
`docs/flock-fiber-deadlock-report.md`, same shape, same pool type).

### 5. Correctness — exact expected value (Pool G, `pm.max_children = 2`)

N = 30 concurrent requests to `counter.php`, all with `PHPSESSID=countertest1`,
against Pool G (two worker processes, 1104981 and 1104982, both
`session.save_handler = files_arb`, same `session.save_path`):

```
counter pid=1104981 n=3
counter pid=1104981 n=9
... (30 lines total, both pids interleaved)
counter pid=1104982 n=30
...
=== final read ===
counter_read pid=1104982 n=30
```

All 30 responses returned **distinct** values covering exactly `1..30`
with no duplicates and no gaps (verified by inspection of the full output —
30 distinct values, min 1, max 30). Final read: `n=30`.

**Verdict: PASS — N=30, final value=30, exact.** No lost updates. This
exercises both the in-process arbiter (fibers on the same pid) and the
underlying kernel `flock()` (contention between pid 1104981 and 1104982)
simultaneously, and both layers held correctly together.

### 6. Cross-process still mutually excluded (Pool H1 + Pool H2, shared `session.save_path`)

Pool H1 (pid 1104988) and Pool H2 (pid 1104994) are separate pools/processes,
both `session.save_handler = files_arb`, both pointing at the same shared
session directory. Cookie `crossproc1`:

```
curl -s -m 30 -b "$COOKIE" http://127.0.0.1:18954/session_a.php?slow_port=18960 &   # H1
sleep 1
curl -s -m 15 -b "$COOKIE" http://127.0.0.1:18955/session_b.php &                    # H2
```

Output:
```
=== H1 (A) ===
A pid=1104988 ... socket_total_ms=5001.1 ... total_ms=5001.6
real  0m5.024s
=== H2 (B) ===
B pid=1104994 session_start_wait_ms=4001.2 total_ms=4001.3 hits=2
real  0m4.015s
```

H2's request (different process, pid 1104994) took ~4.0s — the remaining
hold time — not instant and not stuck forever, confirming it was actually
blocked by the real cross-process kernel `flock()` (the in-process arbiter
only ever governs fibers within its own process; H2 is a different process
entirely, so this contention could only have been resolved by the kernel
lock). It also correctly saw `hits=2`, i.e. H1's write.

**Verdict: PASS** — the in-process lock layers on top of, and does not
replace, cross-process kernel-level mutual exclusion. No silent
cross-process data corruption risk observed.

### 7. Inert for non-`files_arb` handler

The arbiter installs itself unconditionally (once `ext/session` is loaded)
regardless of the pool's configured `session.save_handler` — confirmed
from Pool F-unfixed's own log, which still has `session.save_handler =
files` configured:

```
$ sudo grep coop-session-lock ~/rd/b2/test/log/fpm-f-unfixed.log | tail -1
NOTICE: [pool funfixed] coop-session-lock: in-process session-lock arbiter installed as save handler 'files_arb' (delegates to 'files'); set session.save_handler = files_arb to use it - plain session.save_handler = files is NOT overridden and gets no protection from this feature
```

And measurement #3 above is the direct behavioral confirmation: Pool
F-unfixed (`session.save_handler = files`, arbiter installed but not
selected) still deadlocks exactly like the unmodified baseline — the
installed-but-unselected `files_arb` module has **zero** effect on a pool
that does not opt in.

**Verdict: PASS** — confirmed both from the log (arbiter always announces
itself as installed under the `files_arb` name) and from behavior (a pool
left at plain `files` gets no protection, matching the report's stated
design: opt-in only, per pool).

## Status of measurements

- [x] Build on test box, binary identity verified (`strings` shows our new
      `.c` file paths embedded)
- [x] E1 repro re-run under the arbiter: no deadlock
- [x] Blast radius with the fix (0/20 dead, vs. pre-fix 20/20)
- [x] Correctness: N=30 concurrent increments of a session counter, same id,
      ends at exactly 30
- [x] Cross-process: two worker processes, same session id, still mutually
      excluded (~4s wait, not instant, not stuck forever)
- [x] Confirm inert for `session.save_handler` != `files_arb` (plain `files`
      still deadlocks; arbiter installs but has no effect unless selected)

All six measurements PASS on the fixed binary. One real bug (a
use-after-free causing a reliable SIGSEGV on the first contended in-process
handoff) was found during measurement #2 and fixed — see "Bug found and
fixed during measurement" above; the fix does not change locking/ordering

## Measurements — Variant 2 (`fpm_pool_coop_session_patch.c`)

Build/box: same test box `192.168.8.50`, own directory `~/rd/b2`, own ports
`18951`-`18970` (variant 1's E1/blast/correctness/cross-process pools already
occupied `18951`-`18955` from the earlier pass; variant 2's new pools/slow
servers were placed at `18956`-`18961` and `18965`-`18966` to avoid clashing
with them, all still inside the assigned range). Worktree synced to
`~/rd/b2/fpmng-repo` via `rsync`, then `build/prepare.sh ~/rd/b2/phpsrc` +
`./buildconf --force` (new `.c` file, source list changed — confirmed by
`prepare.sh`'s own warning) + fresh `configure`/`make` in `~/rd/b2/build`
using the same flags as before (`--disable-all --enable-fpmng
--enable-session --enable-pcntl --enable-posix --with-zlib --enable-mbstring
--prefix=/home/piotr/rd/b2/inst`), `make install`.

### Build bug found and fixed (unrelated to locking semantics)

First build failed outright:

```
fpm_pool_coop_session_patch.c:451:17: error: implicit declaration of function
'zend_ini_string_literal'; did you mean 'zend_ini_string_ex'?
fpm_pool_coop_session_patch.c:451:15: error: assignment to 'const char *' from
'int' makes pointer from integer without a cast [-Wint-conversion]
```

`zend_ini_string_literal()` (used in the original draft to read
`session.save_handler` in `psw_check_and_apply()`) is a newer Zend-core
convenience macro (confirmed present in the read-only reference tree at
`/Users/piotr.halas/work/php-src`, in `Zend/zend_ini.h`) that is **absent**
from `Zend/zend_ini.h` in the actual php-src snapshot checked out on the test
box (`~/rd/b2/phpsrc`) — a real version-skew finding, not a typo. Fixed by
calling the underlying `zend_ini_string(ZEND_STRL("session.save_handler"),
0)` directly (same effect, no macro dependency, and no new link dependency
either — `zend_ini_string()` is an ordinary `ZEND_API` function, already
declared in the same header). One-line change,
`fpm_pool_coop_session_patch.c`, `psw_check_and_apply()`. Rebuilt clean after
this (`EXIT=0`, no `error:` lines) and installed successfully.

### 1. Binary identity check

```
$ strings ~/rd/b2/inst/sbin/php-fpm-ng | grep -F 'fpm_pool_coop_session_lock.c'
/home/piotr/rd/b2/phpsrc/sapi/fpmng/fpm/fpm_pool_coop_session_lock.c
fpm_pool_coop_session_lock.c
$ strings ~/rd/b2/inst/sbin/php-fpm-ng | grep -F 'fpm_pool_coop_session_patch.c'
/home/piotr/rd/b2/phpsrc/sapi/fpmng/fpm/fpm_pool_coop_session_patch.c
fpm_pool_coop_session_patch.c
```

Both variants' source paths are embedded. **Verdict: PASS.**

### 2. E1 repro on plain `files`, Pool P (no reconfiguration)

Pool P: port 18956, `pool.executor = fiber`, `pm.max_children = 1`,
`session.save_handler = files` (plain — the whole point of this variant).
Slow server on port 18965 (5s delay).

```
COOKIE="PHPSESSID=v2test2"
curl -s -m 30 -b "$COOKIE" http://127.0.0.1:18956/session_a.php?slow_port=18965 &
sleep 1
curl -s -m 15 -b "$COOKIE" http://127.0.0.1:18956/session_b.php &
```

```
=== A ===
A pid=1213050 connect_ms=0.2 socket_total_ms=5001.2 session_close_ms=0.1 total_ms=5001.4 data='slow-ok'
real  0m5.025s
=== B ===
B pid=1213050 session_start_wait_ms=4003.9 total_ms=4003.9 hits=2
real  0m4.016s
```

Worker state captured mid-hold (2s in, both curls still in flight):

```
$ sudo ps -o pid,ppid,stat,wchan:32,etimes,cmd -p 1213050
    PID    PPID STAT WCHAN                            ELAPSED CMD
1213050 1213048 S    ep_poll                               21 php-fpm: pool p
$ sudo cat /proc/1213050/stack
[<0>] ep_poll+0x496/0x4c0
[<0>] do_epoll_wait+0x58/0xd0
...
```

Same worker pid throughout, no restart, no crash, worker parked in `ep_poll`
(not `locks_lock_inode_wait`). **Verdict: PASS** — plain `session.save_handler
= files`, zero pool reconfiguration, no deadlock.

### 3. Blast radius (Pool P)

Same A/B window (cookie `blastfix1`-style, new cookie used for this run), 20
concurrent `ping.php` fired ~0.5s in while both A and B were in flight:

```
ping 200s: 20/20
```

A and B both completed correctly afterward (`total_ms=5001`,
`session_start_wait_ms=3996.6 hits=2`). **Verdict: PASS** — 0/20 blast
radius, matching variant 1's result, now for plain `files`.

### 4. Correctness (Pool Q, `pm.max_children = 2`, plain `files`)

N = 30 concurrent `counter.php` requests, all `PHPSESSID=v2countertest1`,
against two worker processes (pids 1213263, 1213264), both plain
`session.save_handler = files`:

All 30 responses returned distinct values covering exactly `1..30`, verified
by `sort -n | uniq -c` finding zero duplicates. Final `counter_read.php`:
`n=30`.

**Verdict: PASS — N=30, final value=30, exact.** No lost updates, exercising
both the in-process patch (fibers, same pid) and the underlying kernel
`flock()` (across pids 1213263/1213264) together.

### 5. Cross-process (Pool R1 + Pool R2, shared `session.save_path`, both plain `files`)

Pool R1 (port 18959, pid 1213386) and Pool R2 (port 18960, pid 1213390),
separate processes, same shared session directory, cookie `v2crossproc1`:

```
=== R1 (A) ===
A pid=1213386 ... socket_total_ms=5001.2 ... total_ms=5001.7
real  0m5.025s
=== R2 (B) ===
B pid=1213390 session_start_wait_ms=4001.9 total_ms=4002 hits=2
real  0m4.016s
```

Different pids, R2 waited ~4.0s (the remaining hold time, not instant, not
forever) — the real, cross-process kernel `flock()` is still enforced; the
in-process patch does not (and cannot) replace it. **Verdict: PASS.**

### 6. Inertness for a non-`files` handler (Pool I, `user` handler)

Pool I: default `session.save_handler = files` (a pool default of `user` is
itself rejected — see the correction to measurement setup below), port
18961. `docroot/inert.php` registers a trivial custom handler via
`session_set_save_handler()` whose `read`/`write` callbacks append a marker
line to a fixed file, then calls `session_start()`.

**Setup correction made during measurement**: the pool config was originally
written with `php_value[session.save_handler] = user` as the *default*,
which crash-loops every worker (`Fatal error: Session save handler "user"
cannot be set by ini_set()`, `ext/session/session.c`'s
`OnUpdateSaveHandler` explicitly rejects `user` unless set via
`session_set_save_handler()`, which requires `PS(set_handler)`, which is only
true inside that call, never at ini-registration/MINIT time). Fixed by
leaving the pool's default at `files` and letting the *script itself* call
`session_set_save_handler()` at runtime (the only way `user` is ever legally
selected) — this is not a discrepancy in the variant under test, just an
invalid pool config in the harness, corrected before drawing any conclusion.

```
$ curl -s -b "PHPSESSID=inerttest2" http://127.0.0.1:18961/inert.php
I pid=1219465 save_handler_ini=user x=1
$ cat session_i/marker_read.txt session_i/marker_write.txt
read:inerttest2:1788701803.2011
write:inerttest2:1788701803.2013
```

Markers prove the *custom* handler's own callbacks actually ran (not the
patched `files` wrapper). `ini_get('session.save_handler')` correctly
reports `user`, unchanged by the patch. Log for this pool, checked after the
request, shows only the one container-start NOTICE ("patch variant
installed...") and **no** WARNING/re-apply line — confirming
`psw_check_and_apply()` never fired for this request (it only acts when the
ini string resolves to `"files"`). **Verdict: PASS** — the patch is
genuinely inert for a non-`files` handler, both by log and by behavior.

### 7. `auto_start` gap — measured, and the report's original claim corrected

Pool P-autostart: port 18957, same as Pool P but `session.auto_start = 1`.
New throwaway scripts: `autostart_a.php` / `autostart_b.php` (rely purely on
auto_start, no explicit `session_start()`), `autostart_reopen_a.php` /
`autostart_reopen_b.php` (close the auto-started session, then reopen a
second, explicit one).

**Bug found and fixed (real crash, not a measurement artifact):** the very
first run of the first-half shape (`autostart_a.php` + `autostart_b.php`,
shared cookie) crashed the worker with SIGSEGV, reliably, even with **no
concurrency at all** (`autostart_b.php` alone, solo, fresh worker, crashes on
its very first request). Core dump (`ulimit -c unlimited` for the capture
window only; `core_pattern` pointed at `~/rd/b2/test/log/` and restored to
the box's original `apport` pattern immediately after) + `gdb -batch -ex 'bt
full'`:

```
#0  zend_string_equal_content (s1=0x72da2ec72240, s2=0x1600000002) at Zend/zend_string.h:377
#1  zend_string_equals (...)
#2  ps_files_open (data=0x72da2ec72240, key=0x72da2ec72240) at ext/session/mod_files.c:160
#3  ps_files_write (...) at ext/session/mod_files.c:234
#4  php_session_save_current_state (...) at ext/session/session.c:549
#5  php_session_flush (write=1) at ext/session/session.c:1755
#6  zif_session_write_close (...) at ext/session/session.c:2783
...
#12 fpm_coop_execute (ctx=...) [executing autostart_b.php's source]
```

`data->last_key` reads as garbage (`s2=0x1600000002`). **Root cause**: with
`session.auto_start = 1`, the FIRST `session_start()` runs synchronously
inside RINIT, through the real, unwrapped `files` module — this allocates
`PS(mod_data)` as a raw `ps_files*` (the real module's own struct).
`fpm_coop_session_patch_req_apply()` (called right after RINIT returns, once
per request) was, before this fix, unconditionally swapping `ps->mod = &psw_mod`
at that point **without checking whether a session was already open**. Since
`PS(mod_data)` was left unchanged (still the raw `ps_files*`), the very next
`s_write()`/`s_close()` went through `psw_write()`/`psw_close()`, which
blindly reinterpret `*mod_data` as our own `psw_mod_data*` wrapper struct —
reading `d->locked_key`/`d->inner` at the wrong offsets inside a completely
different struct's memory. Reliable memory corruption, not an edge case:
this hits **every** `auto_start = 1` request that reaches
`session_write_close()`, regardless of concurrency.

**Fix applied** (`fpm_pool_coop_session_patch.c`, `psw_check_and_apply()`):
added a guard — do not swap `ps->mod` while `ps->session_status ==
php_session_active`. This does not touch locking/ordering semantics at all;
it only refuses to swap the module out from under an *already-open* session
(regardless of which module opened it), deferring the swap to the next
check (`req_enter()`, on the next fiber resume) once the session is closed
and there is nothing left to corrupt. Rebuilt, reinstalled, reran solo —
no crash, no core:

```
$ curl -s -b "PHPSESSID=soloB2" http://127.0.0.1:18957/autostart_b.php
B pid=1244764 total_ms=0 hits=1
```

**Separate, pre-existing project-level finding (unrelated to this variant,
NOT fixed here per the task's scope — flagging only):** while diagnosing the
above, discovered that this project's `auto_start` support does not actually
see the request's own cookie. `fpm_coop_req_run()` calls
`fpm_coop_session_request_startup()` (which triggers RINIT, and thus
`auto_start`'s `session_start()`) **before** rebuilding
`$_GET`/`$_POST`/`$_COOKIE`/`$_FILES` for the current request (see
`fpm_pool_coop.c`: the comment on that block explicitly says it runs "right
after" session startup). `php_session_start()` only consults the cookie when
`PS(id)` is `NULL`, and never re-checks it afterward — so the auto-started
session always gets a **fresh, random id**, never the incoming
`PHPSESSID`. Confirmed directly: two sequential requests with the identical
cookie (`PHPSESSID=fixedcookieXYZ`) created two *different* session files,
each `hits=1` (never incrementing). This makes it impossible to force two
concurrent `auto_start`-only requests to collide on the same session id via
a shared cookie at all, in this project's current state — a genuine gap,
independent of variant 2, that changes what could actually be measured for
this scenario. **Not fixed** (out of this task's scope — a change to the
base project's own request lifecycle ordering, not a locking fix); flagged
here so it isn't mistaken for a variant-2-specific limitation.

**First half, as actually measured (given the above):** with a shared
cookie, `autostart_a.php`/`autostart_b.php` never collide on the same
session id (per the finding above), so no deadlock and no contention is
observed — **not** because the fix works, but because the two requests
never touch the same file. This means the report's original prediction
("worker DOES park in `locks_lock_inode_wait`") could **not** be directly
confirmed via a live collision; it remains true by source-reading alone
(`fpm_coop_session_patch_req_apply()`/`req_enter()` cannot run before RINIT
returns, and RINIT is exactly what invokes the auto-started
`session_start()` — confirmed independently by the crash above, which shows
the first call going through the real, unwrapped module's own `ps_files_open()`).
Flagged plainly rather than smoothed over, as instructed.

**Second half — a second `session_start()` after `session_write_close()` —
measured, and the report's blanket claim corrected:** getting a genuinely
*shared* id for the second call also can't use a cookie (same reason as
above, plus: `session_write_close()` does not clear `PS(id)`, so a bare
second `session_start()` would just silently keep reusing the first call's
random id). Used `session_id($shared_id)` instead — the standard, documented
PHP API for choosing an id independent of cookies — in both
`autostart_reopen_a.php`/`_b.php`.

First attempt (`session_write_close(); session_id($id); usleep(50000);
session_start();` — **no intervening I/O**) reproduced the **exact original
deadlock**:

```
$ sudo ps -o pid,ppid,stat,wchan:32,etimes,cmd -p 1244764
    PID    PPID STAT WCHAN                            ELAPSED CMD
1244764 1244762 S    locks_lock_inode_wait                185 php-fpm: pool pautostart
```

Root cause: `fpm_coop_session_patch_req_apply()` runs **exactly once per
request**, at the top, right after RINIT — not on every PHP statement. The
self-heal recheck (`fpm_coop_session_patch_req_enter()`) only runs on an
actual fiber suspend/resume (`fpm_coop_req_enter()`, called from
`fpm_fiber_switch_in()`). `usleep()` does not suspend the fiber (only stream
I/O through the pool's fiber transport does), so with no I/O between the
close and the second `session_start()`, the wrapper is never reinstalled in
time — the second call runs through the real, unwrapped module and deadlocks
exactly like the pre-fix baseline. **This directly contradicts the report's
current wording** ("Any subsequent `session_start()` in the same request...
IS protected") — that claim needed, and has been given, a caveat (see the
correction to the design section above).

Adding a checkpoint `fsockopen()` between the close and the second
`session_start()` (forcing a real suspend/resume) confirmed the wrapper
*does* protect the second call once it has had a chance to reinstall itself:

```
COOKIE-less (session_id() forced), RID=reopenfix2
=== A2 ===
A2 pid=1245629 id=reopenfix2 checkpoint_at_ms=200.9 socket_total_ms=5001.2 total_ms=5202.5 data='slow-ok'
real  0m5.212s
=== B2 ===
B2 pid=1245629 id=reopenfix2 session_start_wait_ms=3796.1 total_ms=3996.9 hits2=2
real  0m4.010s
```

B2 waited ~3.8s (the remaining hold time, in-process, not a kernel block)
and correctly saw `hits2=2`. **Verdict: the "second `session_start()` is
protected" property is real, but conditional** — it requires at least one
fiber suspend/resume to occur between the close and the reopen (any stream
I/O through the pool's transport qualifies; a script with no I/O in between
does not get the benefit and remains as unprotected as the very first
call). The design section above has been corrected to say this explicitly.

### 8. `ini_set()` mid-request self-heal (Pool P)

`docroot/selfheal.php`: `session_start()`/`write_close()` (baseline) →
`session_set_save_handler()` to a trivial `user` handler (internally an
`ini_set('session.save_handler','user')`, `OnUpdateSaveHandler` overwrites
`ps_globals.mod` directly, invisibly to this variant) → `write_close()` →
`ini_set('session.save_handler','files')` (switches back, again invisibly)
→ a short checkpoint `fsockopen()` (forces the suspend/resume that
`req_enter()`'s self-heal check needs) → a second `session_start()`, held
across a slow socket read. `selfheal_b.php`: plain `session_start()`, same
cookie, fired ~2.5s into the hold.

```
=== SH ===
SH pid=1213050 ini_set_at_ms=0.1 checkpoint_at_ms=200.9 hold_start_ms=200.9 ... total_ms=5202.4 hits=1
=== SHB ===
SHB pid=1213050 session_start_wait_ms=2694 total_ms=2694.1 hits=2
$ grep -i coop-session-patch log/fpm-p.log
NOTICE: [pool p] coop-session-patch: in-process session-lock patch variant installed - ...
WARNING: [pool p] coop-session-patch: detected session.save_handler resolving back to the real "files" module
mid-request (an ini_set() bypassed the patch) - re-applying the in-process lock wrapper now (logged once per process)
```

The WARNING fires exactly once, at exactly the trigger the header comment
describes (switch away to `user`, then back to `files`, detected on the
next fiber resume). SHB waited ~2.7s (arrived ~2.5s in, hold released
~5.2s in — consistent with the remaining hold time, in-process, not a
process-wide deadlock) and correctly saw `hits=2` (SH's write). **Verdict:
PASS** — the self-heal is not just "didn't crash", it genuinely
re-establishes protection, confirmed by a real A/B collision around the
reopened session.

## Status of measurements — Variant 2 (`fpm_pool_coop_session_patch.c`)

- [x] Build on test box, binary identity verified (strings shows
      `fpm_pool_coop_session_patch.c` embedded) — **one build bug found and
      fixed** (`zend_ini_string_literal()` macro absent from the box's
      php-src snapshot; switched to `zend_ini_string()` directly)
- [x] E1 repro re-run with PLAIN `session.save_handler = files` (no pool
      reconfiguration): no deadlock, worker never parks in
      `locks_lock_inode_wait` (confirmed: `ep_poll`)
- [x] Blast radius with variant 2 active: 0/20 dead, matching variant 1
- [x] Correctness: N=30 concurrent increments of a session counter, same id,
      ends at exactly 30, no duplicates
- [x] Cross-process: two worker processes, same session id, still mutually
      excluded (~4.0s wait, not instant, not stuck forever)
- [x] Confirm inert for `session.save_handler` = `user` (custom handler's own
      callbacks fire, confirmed via markers; `ini_get()` unaffected; no
      WARNING/re-apply logged) — pool config for this check needed a
      correction (default `session.save_handler = user` is itself invalid;
      fixed to default `files` with the script switching handlers at
      runtime, the only legal way)
- [x] `auto_start` gap: measured. **One real crash-causing bug found and
      fixed** (swapping `ps_globals.mod` out from under an already-open,
      auto-started session corrupts `PS(mod_data)`'s interpretation —
      reliable SIGSEGV, reproduced even solo with no concurrency; fixed by
      not swapping while a session is active). First half (does the FIRST,
      auto-started `session_start()` deadlock on a real collision) could
      **not** be directly confirmed by a live collision — a separate,
      pre-existing, project-level bug (RINIT/auto_start runs before
      `$_COOKIE` is rebuilt for the request) makes two auto_start-only
      requests never actually share a session id via cookie, flagged but
      not fixed (out of scope). Second half (a second, explicit
      `session_start()` after `session_write_close()`) **is** protected,
      but **only when a fiber suspend/resume occurs between the close and
      the reopen** — with no intervening I/O, the second call reproduces
      the full kernel-level deadlock. **This corrects the report's original
      blanket claim** that any subsequent `session_start()` is protected;
      the design section above has been updated to state the condition
      explicitly.
- [x] `ini_set()` mid-request self-heal: WARNING fires exactly once, at the
      documented trigger (switch to `user`, then back to `files`, detected
      on the next fiber resume); confirmed genuinely protective (not just
      "didn't crash") via a real A/B collision around the reopened session
      (~2.7s in-process wait, correct shared counter)

All eight Variant 2 measurements completed. Two real bugs found and fixed
during this pass (both in `fpm_pool_coop_session_patch.c`, both fixed
without changing locking/ordering semantics): the build-time macro-absence
error, and the auto_start use-after-swap memory corruption. One
pre-existing, project-level gap (RINIT running before `$_COOKIE` is rebuilt,
in `fpm_pool_coop.c`) was found and flagged but is out of this task's scope
to fix. Two claims in the original Variant 2 design section were corrected
to match what was actually measured — see the inline corrections above and
in "## Variant 2" itself.

## Shipping decision (branch `feature/session-lock-field-patch`)

Decided: ship **Variant 2 only** (`fpm_pool_coop_session_patch.[ch]`,
"the field-patch"). Reason: `files_arb` (Variant 1) requires opting in via
`session.save_handler = files_arb` in pool config — plain
`session.save_handler = files`, PHP's own stock default, keeps deadlocking
the worker for anyone who does not know to make that change. The
field-patch protects every pool at `session.save_handler = files`
automatically, with zero config change, which is the property that
actually matters for a default that is silently fatal.

`files_arb`'s code (`fpm_pool_coop_session_lock.[ch]`) is **kept in the
tree, not deleted** — the mechanism is real, measured, and may become
relevant again if the field-patch's one structural gap (`session.auto_start`,
see below) ever needs a stronger fix than a startup refusal. It is
**excluded from the build**, not by hand-editing `sapi/fpmng/config.m4`'s
generated `@FPMNG_SOURCES@` list (forbidden — see this project's own
`build/prepare.sh`/`CLAUDE.md`), but by renaming
`fpm_pool_coop_session_lock.c` to `fpm_pool_coop_session_lock.c.notbuilt`:
`find fpm -name '*.c'` no longer matches it, so it is invisible to the
generated source list with no hand-edit anywhere. `fpm_pool_coop_session_lock.h`
is left in place (harmless — nothing includes it once `fpm_pool_coop.c`'s
hooks into the files_arb variant are removed). **NOT SHIPPED. Kept for
reference only** — do not build this file back in without re-reading the
link-dependency risk in "## Verdict" above.

`fpm_pool_coop.c` on this branch hooks only the field-patch variant
(`fpm_coop_session_patch_container_start()`/`_req_apply()`/`_req_enter()`/
`_req_free()`), following the same req_enter/req_free pattern as this
project's existing `ini`/`statics`/`session` features.

### Closing the `auto_start` gap: hard startup refusal

The field-patch's own documented gap (see "The `auto_start` gap" above and
its measurement correction) is closed not by patching around it but by
refusing to start any pool with `session.auto_start = 1` under
`pool.executor = fiber` at all — `fpm_coop_validate()` in
`fpm_pool_coop.c`, following the exact same effective-value-then-refuse
shape as the existing `max_execution_time` check (pool
`php_admin_value`/`php_value` first, `php.ini` fallback otherwise, computed
by a new `fpm_coop_pool_session_auto_start()`, refused with `ZLOG_ALERT`
naming what to change). This is a hard, loud failure, not a silent
downgrade, and it is deliberately NOT limited to the field-patch's own
gap: `session.auto_start = 1` is refused for two independent reasons,
stated together in the refusal message:

1. **Pre-existing, unrelated to locking**: `fpm_coop_req_run()` runs
   `fpm_coop_session_request_startup()` (which triggers RINIT, and thus
   the auto-started `session_start()`) before rebuilding `$_COOKIE` via
   `zend_activate_auto_globals()` — so the auto-started session never sees
   the request's own `PHPSESSID` cookie, ever, on this project's own
   request lifecycle, regardless of any locking fix. This is exactly what
   the RED suite's test 10 already demonstrates (two sequential requests
   with the identical cookie get two different session ids).
2. **Locking**: the same ordering means the field-patch's own post-RINIT
   hook has not run yet either, so the auto-started `session_start()` call
   is unprotected and can still deadlock the worker under concurrency.

`fpm_coop_rejects[]` (the plain fpm.conf-directive rejection list a few
lines above `fpm_coop_validate()` in the same file) does **not** catch
this: `session.auto_start` is a PHP ini setting, reachable via
`php_admin_value[session.auto_start]` in pool config OR via `php.ini`,
neither of which that list's string-match mechanism inspects — hence the
separate, explicit check, mirroring `max_execution_time`'s own handling
for exactly the same reason.
