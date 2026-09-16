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
