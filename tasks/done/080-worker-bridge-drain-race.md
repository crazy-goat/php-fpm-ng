# 080 — Worker bridges can drop a queued request while draining

Status: done
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

## Outcome

**Decision: B then C, as recommended.** Both landed; A stays true but no bridge
in this repo relies on it any more.

**C — `fpmng_worker_may_exit()`** (`fpm_http_direct_worker.c`): true only when
a stop was requested *and* `fw.pending` is empty, i.e. nothing accepted is
still unanswered — queued in `fw.ready` or handed to a handler alike. Both
bridges and `sapi/fpmng/tests/fpmng-http-direct-worker.phpt` deleted their
in-flight counters and the queue-draining workaround task 075 added to the
ReactPHP bridge; the examples got shorter, not longer.
`fpmng_worker_stopping()` is unchanged (option D stays rejected): it is the
earlier, different question.

**B — the SAPI safety net** (`fpm_worker_finish_output()`): on the way out,
before `evhttp_free()`, every still-unanswered request gets a 503 and one
`WARNING` naming the count. The watcher table is destroyed *before* that call
now, because the function drives the base itself and a userland watcher still
registered there would call into PHP after the worker script has returned.

### What the reproduction actually found

Writing the test for B surfaced a second, much more reachable break of the same
invariant, present on `main` and unrelated to any bridge. `evhttp_send_reply()`
only queues the reply on the connection's bufferevent; `evhttp_free()` frees
that bufferevent. **A reply produced in the last loop iteration a worker ever
runs was therefore discarded.** With `pm.max_requests = 1` on the test box that
is every single request:

```
$ curl -sS -D- http://127.0.0.1:28591/probe
curl: (52) Empty reply from server
[08-Sep-2026 17:30:44] NOTICE: [pool probe] child 3927812 exited with code 0
```

No log line, no error — exactly the symptom this task was filed about, reached
without any race at all. The fix is the one the classic transport already uses
for the same reason (`fpm_http_direct.c:118-130`, `:470`): `fw.unflushed`
counts replies handed to libevent, an `on_complete` callback decrements it, and
`fpm_worker_finish_output()` drives the base until it reaches zero or a
1-second budget expires, warning if anything is still unwritten. After the fix
the same request returns `HTTP/1.1 200 OK` / `probed`, and an abandoned one
returns `HTTP/1.1 503 Worker unavailable` with `Connection: close`.

### Tests

`sapi/fpmng/tests/fpmng-http-direct-worker-drain.phpt`, two pools:

- a handler that leaves the loop without answering → the client gets 503, not
  an empty reply; the `WARNING` with the count is in the log; the master
  respawns and the pool keeps serving.
- `pm.max_requests = 1` with `/sleep` in flight while `/probe` answers and
  trips the limit → both get their response, and `/probe` records
  `stopping=true may_exit=false` from inside the handler, which is the state an
  in-flight counter cannot describe.

Full `build/run-fpmng-phpt.sh` on 192.168.8.50 (php-8.5.11-dev, libevent
2.1.12): **PASS=17 FAIL=0 SKIP=8**, binary confirmed with `strings` first.

Both example harnesses stay green against a static-pie build of this branch
(`build/static-full.sh`, Alpine 3.22), also confirmed with `strings`:

- `build/test-http-direct-worker-mysql.sh`: PASS — `concurrent-mysql` 8
  requests in 1 s on one pid (overlap 1.021 s), `concurrent-mysql-tls` the same
  over `TLS_AES_256_GCM_SHA384`.
- `build/test-http-direct-worker-react.sh`: PASS — `concurrent-mysql`,
  `future-tick` (5000 ticks in 8.8 ms, the 10 s blocker never fired),
  `concurrent-tls` (1048576 bytes in 36 reads). Its `tls-strand(keep-alive=1)`
  probe still reports `stranded: 769 of 8192 body bytes after 1 reads of 1024`
  — recorded, non-fatal, and the subject of task 079.

### Review

The Bugbot pass found one real defect in the new code: `fw.unflushed` could
only ever grow. A client that aborts mid-write never reaches the `on_complete`
callback, because libevent frees the request from `evhttp_connection_free()`
while only `evhttp_send_done()` invokes `on_complete_cb` (libevent 2.1.12
`http.c:2773`). From the first aborted request on, every later shutdown —
including the ordinary `pm.max_requests` recycle with nothing actually pending
— would have missed the fast path, blocked the full one-second budget, and
logged a warning about responses nobody was owed. Fixed by also settling the
counter from the connection's close callback: the closecb slot is free for
exactly that window, since every send path clears it immediately before handing
the reply over and libevent does not associate the next request on a keep-alive
connection until `on_complete_cb` has run. Whichever fires first decrements and
clears the closecb, so it cannot happen twice.

The same pass confirmed the `findings.md` `STDERR` entry as task-worthy, with
one detail this task had missed: in the amphp bridge the `fwrite(STDERR, ...)`
sits in a `catch` inside `Amp\async()`, so the `Error` escapes the fiber into
Revolt's uncaught-throwable handler and kills the worker rather than logging.
Filed as task 081.

### Not measured / left out

- The abort path added after review is **not** covered by a test: reproducing
  it needs a client that disconnects while a reply is still queued on the
  socket, which is timing-dependent from outside. The reasoning is anchored to
  libevent's source instead, quoted above.
- The one-iteration race in the original report is **not** reproduced
  deterministically, and cannot be from outside: once stopping is set
  `fpm_worker_accept()` refuses new requests with 503, so the only way to have
  a queued request *and* a stop request is for both to happen inside a single
  `event_base_loop()` iteration. The test asserts the two states the race
  produces — a queued/in-flight request at the moment of the stop decision, and
  an accepted request left unanswered at teardown — rather than the timing.
- A handler that never answers now keeps `fpmng_worker_may_exit()` false for
  ever, i.e. it holds the worker open until the master's stop timeout instead
  of exiting and dropping the request. Deliberate, and documented in the
  builtin's comment; the pending-table saturation path already covers the
  leak's other half.

### Findings recorded (not fixed here)

`STDERR` is **not defined** in this executor — `fwrite(STDERR, ...)` throws
`Error: Undefined constant "STDERR"`. Both bridges report handler failures with
exactly that call, from inside a `catch`, and both READMEs claim it reaches the
FPM log. Measured while debugging the test; filed as task 081.
