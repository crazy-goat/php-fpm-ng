# `pool.type = supervisor`

A supervisor pool runs one script in `supervisor.processes` long-lived
processes and starts it again whenever it ends. The script runs inside a worker
that has already booted the interpreter with OPcache warm, so starting it again
costs a PHP request startup, not a process spawn.

This page is about one thing the directives do not say out loud: who decides how
often the script runs. For the design, the watchdog, the backoff and the
`supervisor.fatal` path, see [`NOTES.md`](NOTES.md) section 3o.

## Who sets the pace

**The script does, and nothing throttles a successful run.** When the script
exits 0 under `supervisor.restart = always`, the next iteration starts
immediately: `supervisor.restart_delay` is the *failure* backoff and does not
apply, and there is no floor of any kind on a successful restart.

That is deliberate. A supervised script is normally a loop of its own — it
blocks on a queue, or sleeps between passes — so the interval between two runs
is a property of the script, and a floor imposed here would be a second,
invisible schedule fighting the one the script already has.
[`pool.type = cron`](cron.md) exists for the case where the schedule belongs
outside the script.

The consequence is that a script which returns immediately is restarted
immediately, forever, at whatever rate the machine allows — measured at 12086
runs per second on one core for a script whose whole body is one
`file_put_contents()`. That is a plausible mistake (`supervisor.script` pointing
at something health-check shaped, or an early `return` left over from
debugging), and it costs a whole core.

## The fast-restart warning

So fpm-ng says so, once, and changes nothing:

```
WARNING: [pool q] supervisor: 1000 consecutive runs of '/srv/q.php' finished in
under 5ms each (about 11900 restarts per second, one core spent on PHP startup
and shutdown). supervisor.restart = always restarts on exit 0 at once, by
design -- the script sets the pace. If it was meant to keep running, it is
returning early; if it was meant to run once, set supervisor.restart = never.
See docs/supervisor.md (issue #122)
```

It is a warning, not a limit. The rate is not capped, no directive was added,
and the pool keeps doing exactly what it did before — the only change is that
the mistake announces itself in `error_log` instead of being visible only as a
busy core. Both thresholds are constants (`FPM_SUPERVISOR_FAST_RUN_MS = 5`,
`FPM_SUPERVISOR_FAST_RUN_STREAK = 1000`): they mark the point at which a
message is worth printing, which is not a policy anyone should have to tune.

The streak is counted in the pool's shared memory, not in the worker's stack,
because there are two ways for the script to start again — the loop going round
and the process dying and being respawned — and only shared state sees both.

The warning fires once per pool and re-arms after any run that takes longer than
the threshold, so a pool that legitimately bursts through short work units and
then settles down says it once rather than every second. Afterwards, the
`restarts` counter on the pool's status and metrics pages
([`operator-endpoint.md`](operator-endpoint.md#the-baseline-counter)) is the
number to watch; the warning is only what tells you to go look.

## Spreading multiple copies apart: `supervisor.restart_jitter`, `supervisor.start_jitter`

(issue #323) Both are **additive on top of** the deterministic behaviour
described above and **default to unset (0)**, which is exactly today's
behaviour: no randomization anywhere, every copy of a pool restarts or cold
starts at the same instant as its siblings.

That lockstep is fine for one process, but `supervisor.processes` > 1 (or
several supervisor pools that happen to depend on the same thing) makes it a
problem: every copy fails at the same instant a shared dependency (a database,
a queue) blips, every copy backs off by the same deterministic
`supervisor.restart_delay`, and every copy retries at the same instant again —
a self-inflicted thundering herd against whatever it was that just recovered.
The same applies to cold start: `supervisor.processes` copies forked together
at master startup all pay their fork+exec+bootstrap cost in the same instant.

**`supervisor.restart_jitter = <seconds>` or `<percentage>%`** adds a random
extra delay, in `[0, jitter]` seconds inclusive, on top of the backoff delay
that `supervisor.restart_delay`/`supervisor.restart_delay_max` already
computed for this failure (including the doubling described above — jitter is
the LAST step, not folded into the doubling itself). Two forms:

- A plain number (`supervisor.restart_jitter = 3`, with the same `s`/`m`/`h`/`d`
  suffixes every other time-shaped directive accepts) adds up to that many
  seconds, regardless of how large the computed delay is.
- A percentage (`supervisor.restart_jitter = 20%`) adds up to that percentage
  of the delay that failure just computed — so the spread grows in proportion
  as backoff itself grows from one consecutive failure to the next, rather
  than becoming negligible next to a delay that has doubled a few times, or
  dominant next to the very first, smallest one.

Jitter never changes `supervisor.restart_max` accounting: it is added to the
delay that is applied, after the consecutive-failures counter and the
give-up decision have already been made from the deterministic delay alone.

The random component is drawn **independently by each copy**, at the moment
that copy is about to wait, rather than computed once (by whichever copy's
failure happened to be most recent) and shared. The deterministic part of the
delay (`restart_delay`/`restart_delay_max`, and the failure count that decides
`restart_max`) is pool-wide state shared by every copy, same as before this
directive existed — but a *random* value stored the same way could only hold
one copy's draw at a time, so every other copy would wait exactly that long
too, collapsing straight back into the lockstep this directive exists to
break. Per-copy jitter draws mean the same shared backoff window is still
respected, but each copy's actual wake-up instant, inside that window, differs
from its siblings'.

**`supervisor.start_jitter = <seconds>`** adds a random extra delay, in
`[0, jitter]` seconds inclusive, before the very first script execution of
each of the pool's `supervisor.processes` copies — spreading the fork+exec+PHP
bootstrap cost across the startup window instead of paying all of it at once.
It applies only to that first execution per copy; every later restart of that
process (or of any process) is `supervisor.restart_jitter`'s job instead. The
pool tracks how many of its `supervisor.processes` cold-start slots have
already been handed out, and consumes one the moment a process decides to
apply (or skip) the delay — not after that process's script has run — so a
process that crashes immediately and is respawned does not draw a second
cold-start allowance just because a slower sibling has not started yet.

Neither directive is seeded from `rand()`/`srand()`: that state is process-wide
and survives `fork()`, so every process forked from the master without an
intervening call in the master would sample the identical "random" value and
collapse straight back into the lockstep these directives exist to break.
Each sample instead comes from the monotonic clock and the calling process's
pid at the moment it is needed, which two calls a nanosecond apart, in any
process, already disagree on.

## `supervisor.max_memory`: a memory-triggered recycle (issue #324)

Classic FastCGI pools bound a worker's memory growth with `pm.max_requests` —
run N requests, then exit and let the master start a fresh copy. A supervisor
pool had no equivalent: the same OS process runs `supervisor.script` over and
over, in the same address space, for as long as `supervisor.restart = always`
keeps deciding to try again, and nothing ever bounded what it accumulated.

`supervisor.max_memory = <size>` (for example `256M`, same K/M/G suffix as
`http.max_body`) closes that gap. After every iteration — right after
`supervisor.script` returns — fpm-ng first runs the normal restart decision
(`fpm_pool_supervisor_apply_policy()`, with the script's real exit code) and
only then, if that decision was "run it again", checks the process's own peak
resident set size (`getrusage(RUSAGE_SELF).ru_maxrss`, converted to bytes; see
the platform-unit comment in `fpm_pool_supervisor.c` for why it does not just
read `ru_maxrss` as bytes on every OS). If that high-water mark has reached the
configured limit, fpm-ng logs one line and exits — `fpm_children.c` (untouched)
respawns a fresh process the same way it always has, which then picks the
script back up (including any `supervisor.restart_delay` backoff already in
effect, since that state lives in shared memory and survives the respawn).

```
NOTICE: [pool q] supervisor: memory usage 268501120 bytes reached
supervisor.max_memory = 268435456 bytes, recycling (not counted as a failure)
```

That "not counted as a failure" is the important part, and it is a property of
*when* the check runs, not of skipping the policy: `fpm_pool_supervisor_apply_policy()`
already treats a run that exited 0 as no failure at all — no touch of the
consecutive-failure counter, no `supervisor.restart_delay` backoff, nothing for
`supervisor.restart_max` to count — regardless of memory. A memory recycle
therefore adds nothing to that accounting in the common case (a script that
keeps completing normally but has grown too big), the same way `pm.max_requests`
recycling a classic worker is not treated as an error either. A run that
*both* exits non-zero *and* is over budget still counts as a real failure —
the memory limit recycles the process either way, but it does not launder a
failing exit code, and `supervisor.restart_max` still means "this many
*actual* failures in a row", not "this many total recycles of any kind".

Running the check only after a "run it again" decision also means
`supervisor.restart = never` and `restart = on-failure` keep their contract: a
copy this pool has decided not to run again parks (instead of exiting and being
handed straight back to `fpm_children.c`, which would otherwise turn every
respawn into another attempt regardless of `supervisor.restart`) rather than
being kept alive by the memory recycle.

**Issue #347:** that decision is **per copy**, not pool-wide. With
`supervisor.processes = N` and `restart = never`, each of the N copies runs the
script exactly once and then parks — "a one-shot job replicated N ways" — and
the pool as a whole is `FINISHED` only once all N have run. Before #347 the
first copy to finish set a pool-wide terminal flag, so the other copies (still
inside their `supervisor.start_jitter` delay, for example) parked without ever
running: N copies produced one execution. `supervisor.restart_max` exhaustion
and `supervisor.fatal` remain pool-wide, as they always were — those describe
the pool giving up, not one copy finishing.

The default is `0` (disabled) — a stock configuration keeps today's unbounded
behavior.

## `supervisor.stop_signal`

`supervisor.stop_timeout` (see [`shutdown-timeouts.md`](shutdown-timeouts.md))
was always paired with an implicit choice of *which* signal starts that grace
period — SIGTERM, hardcoded. `supervisor.stop_signal` makes that a directive:
`TERM` (default, unchanged), `QUIT`, `USR1` or `USR2`. Whichever one is
configured is what a script can additionally `pcntl_signal()` a trap onto to
flush a buffer or close a handle before it exits, and what a memory-triggered
recycle above sends to itself when it decides to stop.

fpm-ng always keeps its own SIGTERM handler installed, regardless of this
directive: `fpm_pctl_kill_all()` (the master's own shutdown escalation,
reference code, unchanged) sends every non-request-serving pool a hardcoded
SIGTERM — it has no notion of a per-pool `stop_signal` and cannot be taught
one without touching that file, so losing this handler whenever
`stop_signal != TERM` would silently turn an ordinary `docker stop`/systemd
stop into an unhandled kill with no `stop_timeout` grace period at all. Setting
`supervisor.stop_signal` to something other than `TERM` installs a SECOND
handler for that signal, in addition to SIGTERM's — it does not replace it. In
every case, `supervisor.stop_timeout` is still the outer bound: if the process
is not gone by then, `SIGKILL` ends it regardless of what the script did or did
not trap.

Both directives are opt-in. Leaving both unset is exactly today's behavior:
no memory bound, `stop_signal = TERM`.

## `supervisor.max_runtime`: a cap on a single iteration (issue #326)

"The script sets the pace" (above) is about *how often* the script runs, not
*how long one run may take* — nothing bounded that, on purpose:
`supervisor.stop_timeout` only ever applies while the pool is being asked to
stop (shutdown, or a `supervisor.max_memory` recycle — see below), never while
it is otherwise healthy and simply taking longer than expected on one pass.

`supervisor.max_runtime = <seconds>` (default: unset/`0`, no cap — the "the
script sets the pace" philosophy is unchanged unless you opt in) closes that
gap the same way `cron.timeout` already does for `pool.type = cron`: it caps a
single execution of `supervisor.script`, not the pool's lifetime or its restart
rate.

Mechanically it reuses the existing pidfd watchdog
(`fpm_pool_watchdog_arm()`, `fpm_pool_watchdog.c`) rather than inventing a
second kill path: right before an iteration starts, a watchdog is armed for
`supervisor.max_runtime` seconds with `supervisor.stop_signal` (`TERM` by
default — see above) as the signal to send if it fires. If the iteration
returns on time, that watchdog is explicitly canceled — unlike `cron.timeout`
(one script run per process) a supervisor process does not end between
iterations, so a stale, un-canceled watchdog from a slow iteration could
otherwise fire during a later, well-behaved one. If instead the watchdog
*does* fire, the stop signal it sends is caught by the exact same handler
already installed for an externally requested stop, which in turn arms
`supervisor.stop_timeout`'s own watchdog with hard `SIGKILL` as the fallback
— so a script that traps the stop signal gets the same grace period an
operator's `docker stop` already gives it, and a script that does not gets
killed outright once `supervisor.stop_timeout` elapses. `fpm_children.c`
(untouched) respawns a fresh process either way, exactly like any other pool
death.

```
[pool q's script overruns supervisor.max_runtime = 5s, ignores the stop
signal, and is SIGKILLed once supervisor.stop_timeout = 10s also elapses —
no log line of its own beyond the two general shutdown-timeout notices,
since the kill happens through the same code path as an external stop.]
```

### Restart-accounting decision

Unlike `supervisor.max_memory` (issue #324), which is *explicitly* exempted
from `supervisor.restart_max`/`restart_delay` — it never sends anything to an
otherwise-finished script, so `fpm_pool_supervisor_apply_policy()` always sees
the script's *own* exit code first and only recycles afterward —
`supervisor.max_runtime` is **not** given that exemption, on purpose:

- If the killed iteration is one `fpm_pool_script_run()` still manages to
  *return from* (the script traps the stop signal and calls `exit()`, with any
  code, before `supervisor.stop_timeout` elapses), `apply_policy()` runs
  completely unmodified with that real exit code — a non-zero exit counts as
  an ordinary failure against `supervisor.restart_max`/`restart_delay`, exactly
  as it would if the script had failed for any other reason. This is the
  intended common case for a script that wants to notice its own timeouts and
  fail loudly.
- If the script neither traps the stop signal nor exits before
  `supervisor.stop_timeout`'s hard `SIGKILL`, the process is gone — there is no
  code left running to call `apply_policy()`, so nothing about that death is
  ever recorded in `supervisor.restart_max`'s counter. This is not a deliberate
  exemption the way `max_memory`'s is (there is no code path that skips the
  check on purpose); it is the same structural fact that already applies to
  *any* process ended by a signal it does not catch — an operator's own
  `docker kill -9`, an OOM kill, a crash — none of which ever reached
  `apply_policy()` either, because that function only ever runs in a process
  that is still alive to call it.

The practical difference from `max_memory`: a script that reliably notices
`supervisor.max_runtime` overruns and fails loudly will correctly trip
`supervisor.restart_max`/`supervisor.fatal` if it keeps timing out — a runaway
script is a real, repeating failure and this cap is meant to surface that, not
hide it behind a free respawn. A script that never notices simply keeps being
killed and respawned outside that accounting, the same as it always would be
if killed by any other uncaught signal; `supervisor.max_runtime` does not
change that pre-existing behavior, it only makes the timeout the *reason* for
this particular kill instead of an operator or the kernel.

Both directives are opt-in. Leaving `supervisor.max_runtime` unset is exactly
today's behavior: no per-iteration cap.

## `supervisor.output_log`: the script's own stdout/stderr, without `catch_workers_output` (issue #328)

`docs/cron.md` covers where `STDOUT`/`STDERR` lead by default (`/dev/null`,
or the master's `catch_workers_output` pipe if that directive is set) and why
the latter is expensive for this pool type specifically: restarting a short
script in a loop is measured at 52 MB of master-side log in 15 seconds (see
the comment in `fpmng-supervisor-restart.phpt`) — every line goes through
`catch_workers_output`'s reader thread on the master, and `supervisor.restart
= always` can mean thousands of restarts a second (the fast-restart warning,
above). `error_log()` is the answer for a script you control; it is not an
answer for a script whose output format is not yours to change, such as a
wrapped third-party binary.

`supervisor.output_log = <path>` is that answer instead: `STDOUT` and
`STDERR` are redirected straight to that file (append, `O_CREAT`, never
truncated) once per process, before the first iteration, bypassing
`catch_workers_output`'s pipe entirely — whether or not `catch_workers_output`
is also set on the same pool. The redirect is process-level, not
per-iteration: the same open file descriptor stays in place across every
`supervisor.restart = always` loop iteration in that process, exactly as a
shell's own `command >>file 2>&1` would, and a fresh one is opened again only
when `fpm_children.c` respawns the process (crash, `supervisor.max_memory`
recycle, backoff-then-retry).

Plain append, no rotation, no size limit — the same expectation as `cron.log`
and any other file this project writes to; rotating it is the operator's own
job. It is also unrelated to any per-run history: unlike `cron.log`,
`pool.type = supervisor` keeps no such log of its own today, so
`supervisor.output_log` is purely the script's raw stdout/stderr, interleaved
across iterations exactly as it was written.

## `fpmng_supervisor_heartbeat()` (issue #327)

The status page only ever shows when the *current* iteration started
(`last_start`) — that tells you nothing once a script does not naturally
return between units of work (a queue-consuming loop, say): "still working"
and "stuck" look identical from the outside for as long as that iteration
runs. `fpmng_supervisor_heartbeat()` lets such a script report its own
liveness periodically, from inside the loop:

```php
fpmng_supervisor_heartbeat();
```

Call it as often as makes sense for your loop — once per unit of work, once
per batch, on a timer, whatever fits. Returns `true` on a successful call.

Returns `false`, doing nothing, when there is no current supervisor context
to record into — the same "nothing to report" convention
[`fpm_connection_info()`](http-direct.md#fpm_connection_info-issue-62) uses.
In practice that means: called outside a `pool.type = supervisor` process.
Like `fpm_connection_info()`, the function only exists in the pool type it
was built for — `function_exists('fpmng_supervisor_heartbeat')` is `false` in
a `cron` pool, an `http`/`http-direct` worker, or a plain FastCGI child, since
it is registered per-pool at startup, not globally. Calling it there is not
possible in the first place; calling it from a supervisor pool before/after
that registration (a saved callable reference, some other code path) still
gets the safe `false` from the runtime guard rather than a crash.

The pool's status page and metrics page expose the result: `heartbeat_age`
(status page) / `fpmng_pool_heartbeat_age_seconds` (metrics page) is the
number of seconds since the last call, present only once the script has
called `fpmng_supervisor_heartbeat()` at least once (a script that never
calls it reports no heartbeat fields at all, rather than a misleading age of
zero or since process start). It lives in the same shared memory as the
fast-restart streak above, so it survives the script returning and being
restarted, *and* survives the process itself being replaced by a new one on
the next restart — it is pool-lifetime state, not process-lifetime state.

That shared memory is keyed by **pool**, not by child: with
`supervisor.processes` > 1, every child of the pool reports into, and reads
back, the same `last_heartbeat` — the age shown is "seconds since any child of
this pool last called `fpmng_supervisor_heartbeat()`", not a separate age per
child. A single stuck child among several healthy ones can therefore still
show a fresh `heartbeat_age`, as long as a sibling keeps calling the function.
Per-child granularity is a real gap, not a documentation nuance — tracked
separately, since it needs its own per-child slot in shared memory rather than
the one pool-wide field this issue added.

This is purely observational, exactly like `cron.expect_within`
([`cron.md`](cron.md#cronexpect_within-issue-327)): nothing here changes
`supervisor.restart`, `supervisor.restart_delay`, or any other control
decision. A stale or missing heartbeat is visible on the status/metrics pages
for a human or an alerting rule to act on; fpm-ng itself never restarts,
kills, or otherwise reacts to one.

## Rolling restart across a reload (issue #329)

A reload in fpm-ng (`SIGUSR2`/`SIGHUP` to the master, or the same signal a
config-file change triggers) is `execvp()`-based: the master signals every
child of every pool, waits for `fpm_globals.running_children` to reach zero
**across the whole master, not per pool**, and only then re-execs itself.
Before this issue, that meant a supervisor pool with `supervisor.processes >
1` — N copies of the same long-running script, kept up for exactly that
redundancy — went through a whole reload with **zero** copies running,
however briefly: every copy got the same signal in the same pass, and the
replacements only started to exist after the master's `execvp()` returned.

Fixing that properly — reload only the pools whose configuration actually
changed, leave the rest running through their own `execvp()`-free lifetime —
is a different, much bigger change (selective reload, tracked separately;
see `docs/NOTES.md`, "Hot-reload scope"). It fights the exec-based
architecture: the master itself is one process image, so "leave pool A alone"
still has to survive that same image being replaced. This issue does not
attempt it. What it does instead is narrower and works within today's
architecture: **keep exactly one already-running copy of the script alive
across the reload**, so the pool's redundancy never drops to zero, even
though every *other* copy still restarts together, the same way the whole
pool always has.

**Mechanism.** On a reload's first signal pass, before the master's normal
per-child signal loop reaches a supervisor pool's children, the pool detaches
one of them — the one that has been running longest, on the theory that the
copy least likely to be moments from crashing or self-recycling
(`supervisor.max_memory`/`supervisor.max_runtime`) on its own gives the
survivor window its best chance of actually covering the gap — from the
ordinary `pm.*`-counted bookkeeping the reload's "wait for zero, then exec"
gate watches. That detached process is never signalled by this reload; it
keeps running the *old* generation's script, untouched, while everything else
goes through the signal-then-`execvp()` sequence as before.

`execvp()` keeps the master's environment (`environ` survives; the
`mmap(MAP_ANONYMOUS)` shared memory this pool type otherwise uses for all its
other state, per `docs/NOTES.md` section 3p, does not), so the survivor's pid
is handed to the new generation through a single environment variable, one
`pool-name:pid` pair per pool that spared a child. The new generation reads
its own pool's entry back out on startup, confirms the pid is still alive,
and starts watching it — it is not a child this generation ever forked, so it
is tracked the same way `pool.type = http`'s gateway processes are (a
generic master-side "notice this pid's exit, but it is not a `pm.*` child"
registry), not through the normal worker-respawn path.

**Retiring the survivor.** The new generation's own fresh copies are already
running by the time it reaches this point — `fpm_children_create_initial()`
runs earlier in startup — so the survivor is redundant capacity from the
moment the new generation exists at all; the only question is when it is safe
to stop it. "Safe" here means *a replacement has actually started*, not just
*been forked*: the new generation polls its own shared `starts` counter
(bumped at the top of each script iteration, before the script itself runs —
the same counter the pool's own restart/backoff accounting already
maintains) and retires the survivor, with `supervisor.stop_signal`, the first
time it sees that counter move. If nothing ever does — a broken script that
never gets past its own startup — the survivor is retired unconditionally
after 30 seconds anyway, so a stuck new generation cannot leak the old one
forever.

**What this does not cover:**

- **`supervisor.processes = 1` still has a real gap.** With exactly one copy,
  "spare one and keep going" would leave the pool running the *old*
  generation's script for the entire reload and never start the new one at
  all — worse than today's brief gap, not better. This mechanism only
  activates when the pool has at least two running children at the moment of
  reload; a single-process pool reloads exactly as before.
- **The survivor's own stdout/stderr stop being forwarded once the old
  master execs away** — that forwarding is master-side plumbing
  (`fpm_stdio.c`), and the old master is gone. `supervisor.output_log`
  (issue #328) is unaffected, since that file descriptor belongs to the
  child process itself, not to the master's pipe.
- **A pool renamed or removed in the very same reload that spared one of its
  children** leaves that pid's entry in the environment variable with no pool
  left to claim it on the next startup — `fpm_pool_supervisor_reload_survivor_env_take()`
  is only ever called with the name of a pool that still exists in the new
  config, so a stale entry for a name nobody asks for is never read, never
  tracked, and therefore never reaches the 30-second unconditional-retirement
  path either: that path only starts once `fpm_pool_supervisor_reload_survivor_track()`
  has registered the poll timer for it. The leftover process keeps running the
  old generation's script until something external notices and kills it — a
  real, known gap, not one this issue closes. It needs the rename/removal and
  a reload to land in the exact same config change, which is narrow enough
  that a follow-up (a startup-time sweep that kills any env-var entry no pool
  claimed) was left for a separate issue rather than built speculatively here
  (see `findings.md`).
- **Selective reload** — skipping *unrelated* pools entirely on a reload that
  only changed one of them — is issue #330's scope, not this one's. This
  issue is useful on its own even without it, per the original request: it
  closes the zero-copies window for a supervisor pool's own
  `supervisor.processes` change or an ordinary reload, regardless of whether
  selective reload ever lands.
