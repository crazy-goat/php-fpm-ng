# 080 — Worker bridges can drop a queued request while draining

Status: open
Type: bug
Depends on: 073 (merged)
Related: `sapi/fpmng/fpm/fpm_http_direct_worker.c:342-347`, `:770-776`

## Why

Both userland bridges decide the worker has drained from their own in-flight
counter alone:

- `examples/http-direct-worker/FpmngServer.php:88-96` (amphp)
- `examples/http-direct-worker-react/FpmngReactServer.php` (ReactPHP; fixed
  there in task 075, so that file is the reference for the fix)

That counter only tracks requests `fpmng_worker_next_request()` has already
handed over. The SAPI has a queue behind it — `fw.ready`
(`fpm_http_direct_worker.c:342-347`) — and a request sitting in that queue is
invisible to the bridge.

The sequence that drops one, all inside a single `event_base_loop()`
iteration:

1. Request A is in flight, `inFlight == 1`; the notify pipe was not readable
   when the loop last polled.
2. evhttp accepts request B: `fpm_worker_accept()` queues it in `fw.ready` and
   writes the notify byte. The read watcher will not be polled again until the
   next iteration.
3. Still in the same iteration, A's response is sent — and `respond()` itself
   is what trips `pm.max_requests` and sets `fpm_worker_stopping`
   (`fpm_http_direct_worker.c:770-776`). A SIGQUIT arriving any time before
   this does the same.
4. A's completion handler sees `stopping == true` and `inFlight == 0`, cancels
   the notify watcher and stops the loop.
5. The script ends and `evhttp_free()` closes B's connection with **no
   response**: the client gets an empty reply and nothing is logged.

`pm.max_requests` makes this reachable under ordinary load rather than only on
reload, because step 3 needs no external signal at all. The window is one loop
iteration, so it is rare and will present as an unexplained empty reply.

## Scope

- Fix `examples/http-direct-worker/FpmngServer.php` the way task 075 fixed the
  ReactPHP bridge: before concluding the worker is drained, drain the SAPI
  queue (`while (($id = fpmng_worker_next_request()) !== null)`) and only tear
  down if that yielded nothing *and* nothing is in flight.
- Check `sapi/fpmng/tests/fpmng-http-direct-worker.phpt` for the same
  assumption and fix it if present.
- Decide whether this belongs in the examples at all or whether the SAPI should
  expose the queue depth (or make `fpmng_worker_stopping()` false while
  `fw.ready` is non-empty), so that every future bridge does not have to
  rediscover it. A bridge author cannot be expected to know about `fw.ready`.

## Options

Worth noting first: userland does **not** need to stop accepting requests
itself, because `fpm_worker_accept()` already answers 503 and refuses once
`fpm_worker_stopping` is set (`fpm_http_direct_worker.c:305`). So the only real
use a bridge has for `fpmng_worker_stopping()` is deciding *whether it may
exit* — which is precisely the question it cannot currently answer.

**A. Fix each bridge in userland.** Ask `fpmng_worker_next_request()` once more
before concluding, which is what task 075 did for the ReactPHP bridge. This has
to stay as the documented idiom, but it does not scale: every bridge author must
know that `fw.ready` exists, i.e. must know our internals. A third bridge will
make the same mistake.

**B. A safety net in the SAPI (recommended).** On the way out, before
`evhttp_free()`, walk `fw.pending` and send 503 with `Connection: close` to
every request that was never answered, plus one log line with the count. The
invariant becomes: *we never close an accepted connection without a response.*
About twenty lines, in the spot that already carries the comment about
`evhttp_free()` and close-callback ordering (`fpm_http_direct_worker.c:
1240-1249`). No bridge can bypass it — it also covers third-party bridges, a
handler that threw, and an `exit()`. It converts a silently dropped request into
an honest 503 that appears in the log.

**C. A builtin that answers the real question (recommended).** The SAPI already
knows everything: `fw.pending` holds *all* unanswered requests — both those
queued in `fw.ready` and those handed to a running handler. So
`fpmng_worker_may_exit()` is `fpm_worker_stopping && pending is empty`. With it,
a bridge can **delete its own in-flight counter** and needs to know nothing
about the queue. Caveat: a handler that never answers would block the exit
for ever — bounded in practice by the master's own stop timeout, and there is
already separate handling for that leak (the 503 saturation path at `:305-328`).

**D. Redefine `fpmng_worker_stopping()`** so it only returns true once the queue
is empty (rejected). One name for two different questions: an application may
legitimately want to know about the shutdown early, to close pools or flush
metrics.

**Recommendation: B then C.** B is cheap and catches the cases we have not
thought of; C removes the reason the bug was possible at all. A stays as the
documented idiom for as long as bridges carry their own counters.

## Acceptance criteria

- A test that reproduces the drop before the fix: with `pm.max_requests` set
  low, drive requests concurrently so one is queued at the moment the last
  in-flight response trips the limit, and assert every request got a response.
  "No empty replies under a recycling worker" is the observable.
- Both examples' harnesses stay green (`build/test-http-direct-worker-mysql.sh`,
  `build/test-http-direct-worker-react.sh`).
- If the SAPI-side option is chosen, the examples get simpler rather than both
  carrying the same loop.

## Out of scope

- Task 067's per-worker retire trigger. That is a new master-side mechanism;
  this is an existing path dropping a request that was already accepted.
- Connection-level policy (task 063).
