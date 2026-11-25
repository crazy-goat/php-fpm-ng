# Timers: which callback may delete which event

This note states the rule for timer callbacks in `sapi/fpmng/fpm/`. Read it before you add a timer. Read it also before you call `fpm_event_del()` from a callback, or free memory that holds a timer. Issue #1012 has the history.

## Why the rule exists

The timer walk in `fpm_event_loop()` is in `third_party/php-src/sapi/fpm/fpm/fpm_events.c`, lines 429-466. For each node, the walk does this:

1. It saves `q->next` in a local variable (`fpm_events.c:432`).
2. It fires the node. A one-shot node is unlinked and freed before its callback runs (`fpm_events.c:440-456`). A `FPM_EV_PERSIST` node stays queued.
3. It reads only the saved pointer after the callback. It also checks `fpm_globals.parent_pid` (`fpm_events.c:458-466`).

A callback that deletes the saved node frees it. The walk then reads freed memory. The walk also reads `q->ev` for each node (`fpm_events.c:434-436`). The queue node keeps a pointer to the `struct fpm_event_s` (`fpm_events.h:24-28`). So memory that holds a queued event must stay valid until the event is deleted.

## The rule

1. A timer callback can call `fpm_event_del()` only on the timer that fired.
2. A timer callback must not call `fpm_event_del()` on any other queued timer.
3. A callback that runs inside a timer callback follows rule 2. `fpm_children_bury()` runs inside a timer callback. `fpm_postponed_children_bury()` calls it (`fpm_events.c:55-57`), and SIGCHLD arms that timer (`fpm_events.c:86-87`). The exit callback is run by `fpm_children_extra_handle_exit()` (`fpm_children_extra.c:50`), which `fpm_children_bury()` calls (`fpm_children.c:493`).
4. Do not free memory that holds a queued `struct fpm_event_s`. Delete the timer first, and only when rule 1 allows it. `fpm_pool_supervisor_reload_survivor_poll()` deletes `poll_ev` before it frees the survivor (`fpm_pool_supervisor.c:923-924`).
5. To stop another queued timer, set a flag. The timer deletes itself when it next fires. Example: `reaped` and `poll_armed` in `fpm_pool_supervisor.c`.
6. To change the deadline of a queued timer, call `fpm_event_add()` on it. Do not delete it and add it again.

## Where the rule is used

| Site | Action | Verdict |
|---|---|---|
| `fpm_pool_supervisor.c:923` (poll, reaped branch) | Deletes `poll_ev`, the timer that fired, then frees the survivor | Rules 1 and 4 |
| `fpm_pool_supervisor.c:845` and `:851` (retire) | Delete `poll_ev` | Allowed only because `retire()` is called from the poll alone (`fpm_pool_supervisor.c:940`, `:948`). A caller from another callback breaks rule 2 |
| `fpm_pool_supervisor.c:813-832` (exit callback) | Never deletes a timer. Sets `reaped` and returns while a timer is queued | Rule 2 |
| `fpm_adopted_sweep.c:47-51` and `:155` | Deletes `sweep_event` from `fpm_adopted_sweep_tick()`, the timer that fired | Rule 1 |
| `fpm_crash_backoff.c:46-49` | Never deletes `gate_ev` or `survive_ev`. Re-arms them in place | Rule 6 |
| `fpm_children.c:96`, `:100`, `fpm_stdio.c:290` | Delete fd events | Outside this rule: fd events are not in the timer queue (`fpm_events.c:538-549`) |

## Checking a change

Run this command after any change to a timer callback:

```sh
grep -rn "fpm_event_del" sapi/fpmng/fpm/
```

For each hit, decide whether the callback is the timer that fired (rule 1) or another event (rule 2). An fd event is not in the timer queue, so it is outside this rule.

## Why the normal start path does not reach the bad order

The bad order is `poll_ev` as the node saved after `children_bury_timer`. By code reading, the normal start path cannot produce it:

1. `poll_ev` is queued only by `fpm_pool_supervisor_reload_survivor_track()` (`fpm_pool_supervisor.c:961`). Its only caller is `fpm_pool_supervisor_init_main()` (`fpm_pool_supervisor.c:1442`).
2. `init_main()` runs from `fpm.c:201`, before `fpm_event_loop(0)` (`fpm.c:259`).
3. The loop queues the PERSIST idle-maintenance heartbeat when `err` is 0 (`fpm_events.c:370-371`). Nothing deletes it.
4. `fpm_event_queue_add()` inserts at the head and does not move a node that is already queued (`fpm_events.c:161-193`).
5. So the heartbeat stays between `poll_ev` and every node queued later, including `children_bury_timer` (`fpm_events.c:86-87`). In an `err` = 0 run, the saved node after `children_bury_timer` is never `poll_ev`.

The `err` = 1 paths (`fpm.c:177-235`, no heartbeat) were not analysed. A crash there is not known.

## Not measured

The race was not reproduced. A probe on the build before the fix read 0 in the exit callback. That result means the bad order did not occur in one run of the test. A clean valgrind run on a scenario that does not reach the bad order does not show that the fix works. It shows only that the new path has no invalid access. The valgrind counts are in the pull request for issue #1012, not in this repository.

A deterministic reproduction needs a test-only fault point that re-queues `poll_ev` inside the loop. Queueing another timer does not help, because the new timer goes in front of `poll_ev`.
