# Timers: which callback may delete which event

This note states the rule for timer callbacks in `sapi/fpmng/fpm/`. Read it before you add a timer, or before you call `fpm_event_del()` from a callback. Issue #1012 has the history.

## Why the rule exists

The timer walk in `fpm_event_loop()` is in `third_party/php-src/sapi/fpm/fpm/fpm_events.c`, around lines 430-460. The walk does this for each node:

1. It saves `q->next` in a local variable.
2. It fires the node's callback. A one-shot node is unlinked and freed before the callback runs. A `FPM_EV_PERSIST` node stays queued.
3. It continues with the saved pointer.

The saved pointer is not updated when a callback deletes another node. If a callback deletes the saved node, the walk reads freed memory. This is a use-after-free. The code cannot show it, because the saved node is a local variable in the vendored loop.

## The rule

1. A timer callback can call `fpm_event_del()` only on the timer that fired. The loop does not read the fired node again, except to check `parent_pid`.
2. A timer callback must not call `fpm_event_del()` on any other queued timer.
3. A callback that runs inside another timer callback follows the same rule. Example: an exit callback that runs from `fpm_children_bury()`, which is itself a timer callback (`fpm_children_extra.c:50-64`).
4. To stop another timer, set a flag and let that timer delete itself when it next fires. Example: `reaped` and `poll_armed` in `fpm_pool_supervisor.c`.
5. To change the deadline of a queued timer, call `fpm_event_add()` on it. Do not delete and add it again.

## Where the rule is used

| Site | What it does | Status |
|---|---|---|
| `fpm_pool_supervisor.c`, `fpm_pool_supervisor_reload_survivor_poll()` | Deletes `poll_ev`, which fired | Allowed (rule 1) |
| `fpm_pool_supervisor.c`, `fpm_pool_supervisor_reload_survivor_exited()` | Sets `reaped` and returns. The queued timer deletes itself | Required (rule 2) |
| `fpm_adopted_sweep.c`, `fpm_adopted_sweep_disarm()` | Deletes `sweep_event` from `fpm_adopted_sweep_tick()`, which fired | Allowed (rule 1) |
| `fpm_crash_backoff.c:46-49` | Never deletes `gate_ev` or the other timer. Re-arms in place | Required (rule 5) |
| `fpm_children.c:96,100`, `fpm_stdio.c:290` | Delete fd events, not timers | Outside this rule |

## Checking a change

Run this audit after any change to a timer callback:

```sh
grep -rn "fpm_event_del" sapi/fpmng/fpm/
```

For each hit, decide whether the callback is the fired timer (rule 1) or another event (rule 2). An fd event is not in the timer queue, so it is outside this rule.

## Not measured

The race needs `poll_ev` to be the node after `children_bury_timer` in the queue. Issue #1012 has a valgrind run of the reload survivor test on the old and the new build. Neither run reached the bad case, and both runs had no invalid read. The rule rests on the code reading of the loop, not on a reproduced crash.
