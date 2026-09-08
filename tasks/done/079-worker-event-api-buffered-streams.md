# 079 — Worker event API: streams whose data is invisible to libevent

Status: done
Type: decision + implementation
Depends on: 073 (merged), 075 (merged)
Related: `sapi/fpmng/fpm/fpm_http_direct_worker.c:824-827`, `tasks/done/075-*.md`

## Why

`fpmng_worker_event_create()` arms a libevent watcher on a *descriptor*, while
userland works on a *PHP stream*. When the stream has a userland read buffer —
TLS, or any filter — bytes can sit in that buffer while the descriptor is
genuinely empty, so no readability event will ever fire for them. The code
knows this and says so, but only in a comment, and only on the path where the
stream has no usable descriptor at all (`fpm_http_direct_worker.c:824-827`) —
a TLS stream does have one, so it sails past.

Task 075 measured the consequence rather than predicting it. Over TLS with
`Connection: keep-alive`, an 8 KiB response, one read per readable event:

| read chunk | result                                          |
| ---------- | ----------------------------------------------- |
| 512        | stranded, 257 of 8192 body bytes after 1 read   |
| 1024       | stranded, 769 of 8192 body bytes after 1 read   |
| 4096       | stranded, 3841 of 8192 body bytes after 1 read  |
| 8192       | stranded, 7937 of 8192 body bytes after 1 read  |
| 65536      | complete, 8447 bytes in 1 read                  |

Stranded means for ever: the application waits on a descriptor that will never
be readable again. Two things make this worse than a documentation gap.
Matching PHP's default `chunk_size` of 8192 does not help, because the
stranded amount is whatever the peer happened to send; and the failure is
silent, with no log line and no error — the request simply never finishes.
`react/http` and `amphp/byte-stream` both happen to avoid it (a 65536-byte
chunk, and a direct read before arming a watcher, respectively), which is
exactly why nobody hit it in 073 or 074.

## Scope

Two separable questions. **Decide which this task answers, and say so before
implementing** — they have different costs and the first may be enough:

1. **Tell the truth at the boundary.** Should `fpmng_worker_event_create()`
   refuse, or warn about, a stream that carries userland buffering? A stream
   with filters is detectable; a TLS stream is detectable. Refusing outright
   would break `amphp/byte-stream`, which uses such streams correctly, so a
   warning or an opt-in flag is the more likely shape.
2. **Make the loop tell the truth instead.** Should `fpmng_worker_loop()`
   treat a stream with a non-empty userland buffer as ready, and invoke its
   read watcher without waiting for the descriptor? That removes the trap
   rather than documenting it, at the cost of the loop having to know about
   every registered stream's buffer state on every iteration.

Either way the application-side rule — **read until the read comes up short,
not once per readable event** — belongs somewhere an application author will
see it: `sapi/fpmng/README.md` or `docs/http-direct-revolt-integration.md`, not
only in a C comment and a task Outcome.

## Root cause, found after the task was filed

We cast the stream to a descriptor **once**, in `fpmng_worker_event_create()`
(`fpm_http_direct_worker.c:821-823`), and keep the `fd`. PHP's own
`stream_select()` casts **on every call** (`ext/standard/streamsfuncs.c:673`) —
with the same `PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL` flags we
use. That matters because for an SSL stream the cast is not a passive lookup:
it pulls `SSL_pending()` bytes out of OpenSSL into PHP's own read buffer
(`ext/openssl/xp_ssl.c:3874-3886`).

So PHP has exactly our problem and solves it by repeating the cast. We do it
once and never again, which is why bytes that arrive inside a TLS record burst
become permanently invisible. This is a much better starting point than
"document the trap".

## Options

**A. Repeat what `stream_select()` does (recommended).** In
`fpmng_worker_loop()`, before sleeping, walk the read watchers; for each, cast
the stream again (draining `SSL_pending()` as a side effect) and check whether
PHP's read buffer holds anything. If it does, call `event_active()` on that
event — telling libevent the event just happened. Libevent then dispatches it
in this iteration and does not sleep, so the `$blocking` argument stops being
dangerous. The trap disappears rather than being documented, and the strategy
is PHP's own rather than invented here.

Two details that decide whether this works:

- *The detector.* `stream->has_buffered_data` (`main/php_streams.h:215`) cannot
  be used despite the name: it is cleared at the end of `php_stream_read()`
  (`main/streams/streams.c:685`), so from outside it says nothing. Test the
  buffer itself — `writepos > readpos`.
- *Busy-spin risk.* The semantics become level-triggered: an application that
  reads nothing would have its watcher fired every iteration and burn CPU. That
  is a trade of a silent hang for a loud spin, which is the better trade, but it
  needs a counter — if the buffer has not shrunk after N iterations, log a
  warning naming the pool. 100% CPU with nothing in the log would be worse than
  the hang.

Cost is one cast plus one field read per read watcher per iteration, on a
handful of watchers. Expected to be negligible; measure it rather than assume.

**B. Warn or refuse at `event_create()` (rejected).** Refusing breaks
`amphp/byte-stream`, which uses such streams *correctly* by reading directly
before arming a watcher. And a warning at create time concerns a state that does
not exist yet — the buffer is empty then. It cannot distinguish a correct
consumer from a doomed one.

**C. Expose `fpmng_worker_stream_has_buffered($stream)` (worth adding, not
instead).** Lets a driver author do amphp's direct-read trick generically. Good
as a companion to A, insufficient alone: it still requires knowing the trap
exists.

**D. Documentation only (rejected as the whole answer, required either way).**
Cheapest, and both libraries already cope — but it leaves a class of bug whose
symptom is "the request never finishes and nothing is logged". The application
rule (**read until the read comes up short, not once per readable event**) must
be documented regardless of which option is implemented.

## Acceptance criteria

- The decision between (1) and (2) — see Options above, where A is (2) and B is
  (1) — is written down with its reason, including what it would break, before
  any code changes.
- A regression test that strands a buffered TLS stream and asserts the chosen
  behaviour, plus, if option A is taken, one that proves the busy-spin guard
  fires rather than the worker spinning silently. `examples/http-direct-worker-react/`'s `/strand` route plus
  `build/test-http-direct-worker-react.sh` already reproduce it end to end and
  can be reduced to a `.phpt`, or driven as-is.
- `amphp/byte-stream`'s direct-read pattern keeps working unchanged: task 074's
  harness stays green.
- The application-side rule is documented where an author will find it, with
  the measured numbers above rather than a general warning.

## Out of scope

- Rewriting either example to work around the trap; both already do.
- Buffering on the *write* side.
- Anything about `pool.type = http-direct` in classic (non-worker) mode.

## Outcome

**Decision: A, with C as a companion. B and D-alone rejected, as recommended.**
Written down before any code changed, in the Options section above; nothing in
that reasoning had to be revised while implementing it.

**A — the loop looks inside userland read buffers**
(`fpm_http_direct_worker.c`, `fpm_worker_activate_buffered()`). Once per
`fpmng_worker_loop()` iteration, *before* `event_base_loop()`, every pending
read watcher's stream is cast again with the same
`PHP_STREAM_AS_FD_FOR_SELECT | PHP_STREAM_CAST_INTERNAL` flags PHP's own
`stream_select()` uses (`ext/standard/streamsfuncs.c:673`). For an SSL stream
that cast is the operation that moves `SSL_pending()` bytes into PHP's read
buffer, so it is a fetch and not a lookup. If the buffer then holds anything
(`writepos > readpos` — not `stream->has_buffered_data`, which
`php_stream_read()` clears on the way out), the watcher gets `event_active()`:
libevent dispatches it in this iteration and computes a zero timeout, so
`$blocking = true` can no longer park the worker on a descriptor that will
never be readable again.

**C — `fpmng_worker_stream_has_buffered($stream)`**, for a driver that would
rather decide for itself than be told by an activated watcher. It shares
`fpm_worker_stream_buffered()` with the loop, so the re-cast is part of it on
purpose: a `false` means the bytes are genuinely not there yet, not merely
not fetched.

**The busy-spin guard the task asked for.** These semantics are
level-triggered, so a consumer that reads nothing would have its watcher fired
every iteration. `FPM_WORKER_SPIN_LIMIT = 100` consecutive activations with a
buffer that never shrank produce one `WARNING` naming the pool and the byte
count, once per spin rather than once per iteration. A shrinking buffer is the
only available evidence of progress; "the watcher fired again" means nothing
under level-triggered semantics.

### Measured

Test box, static-pie build of this branch, `strings` confirming the new
literal is in the binary under test.

- **phpt:** `PASS=18 FAIL=0 SKIP=8`, including the new
  `sapi/fpmng/tests/fpmng-http-direct-worker-buffered-streams.phpt`.
- **Negative control, which is what makes that test a regression gate rather
  than a decoration.** With `fpm_worker_activate_buffered();` commented out and
  nothing else changed, the new test fails:
  `strand: got 1024 of 8192 bytes in 0 watcher call(s)`.
- **The trap itself, end to end.** `/strand` against a TLS origin that holds
  the connection open, at a 1 KiB read chunk:
  `{"before_read":false,"first_read":1024,"buffered_after_short_read":true,"bytes":8192,"reads":7}`
  — all 8192 bytes, where task 075 measured 769 of 8192 after one read.
- **A watcher over a closed stream.** The `/closed` route closes the stream
  before freeing its watcher and then runs five loop iterations: all five run
  and no exception is raised. It fails on the first review defect below, which
  is why it exists.
- **The guard.** `/spin` returns `{"spins":200}` and the log carries exactly one
  line: `WARNING: [pool buffered] http-direct worker: a read watcher was
  invoked 100 times in a row with 7168 byte(s) left in the stream's userland
  buffer and nothing consuming them, so the loop cannot sleep. Read until the
  read comes up short, not once per readable event`.
- **`build/test-http-direct-worker-react.sh`: PASS**, with
  `tls-strand(keep-alive=1): ok (8192 body bytes in 9 reads of 1024)` where
  task 075 recorded `STRANDED (769 of 8192 body bytes after 1 reads of 1024)`.
  That probe was a recorded measurement in 075 and is now a hard `fail` gate,
  which is the second regression test asked for.
- **`build/test-http-direct-worker-mysql.sh`: PASS**
  (`concurrent-mysql-tls: ok ... cipher TLS_AES_256_GCM_SHA384`), so
  `amphp/byte-stream`'s direct-read pattern keeps working unchanged and task
  074's harness stays green — an acceptance criterion, and the reason option B
  was rejected.
- **Cost, measured rather than assumed.** The `future-tick` route drains 5000
  `futureTick()` callbacks, i.e. 5000 loop iterations each now carrying the
  extra walk: `8.8ms`, `7.1ms` and `8.2ms` across three runs of this branch,
  against `8.8ms` recorded by task 080 before the change — i.e. inside the
  run-to-run spread. Caveat worth stating: that harness has a handful of
  watchers, so it bounds the per-iteration constant, not the behaviour of a
  worker holding thousands of streams.

### Documentation

The application-side rule — **read until the read comes up short, not once per
readable event** — now has its own section in
`docs/http-direct-revolt-integration.md` with task 075's measured table rather
than a general warning, plus the `stream_select()` precedent and the
consequences (level-triggered read watchers, the named spin, the new builtin).
Both example READMEs were corrected where they said the trap was still open:
`examples/http-direct-worker-react/README.md`'s read-chunk trap section now
opens by saying task 079 closed it and keeps the measurements as motivation,
and its application rule is qualified as being about CPU rather than
correctness.

**Filters are covered by the same mechanism**, which took two goes to get
right and is documented in the same section. The first draft of the new test
used read filters as a TLS stand-in and the worker exited 255 on every boot; the
diagnosis at the time was that `php_stream_cast()` refuses filtered streams
outright, and that diagnosis was wrong. `main/streams/cast.c:307` reads
`if (php_stream_is_filtered(stream) && castas != PHP_STREAM_AS_FD_FOR_SELECT)`
— the refusal is skipped for precisely the cast this code uses, so a filtered
socket stream *is* accepted by `fpmng_worker_event_create()`, and since a
filter's output lands in the same `readbuf` the detector inspects, it is
activated for its buffered bytes like any other. The test was rewritten around
a real TLS origin regardless, because that is the case task 075 measured.

### Review

Six issues in this branch's own new code, five of them fixed here, all six
verified against php-src rather than argued about.

1. **A watcher over an `fclose()`d stream would have killed the loop for
   good.** `php_stream_from_zval_no_verify()` expands to
   `zend_fetch_resource2_ex(zv, "stream", ...)`, and for a resource whose type
   `zend_resource_dtor()` has already reset to `-1` that *throws* a `TypeError`
   rather than returning NULL (`Zend/zend_list.c`). Thrown from the new
   per-iteration walk, the exception left `fpmng_worker_loop()` through
   `RETURN_THROWS()` before `event_base_loop()` ran; since the watcher stays
   registered and pending, every later call threw the same error, so the worker
   answered nothing ever again — while blaming an argument the caller never
   passed. Reachable from any handler that closes a stream before freeing its
   watcher. Fixed with `fpm_worker_watcher_stream()`, which passes a NULL type
   name so the fetch returns NULL quietly, and covered by the new `/closed`
   route in the phpt.
2. **The refill can re-enter PHP, and the walk assumed it could not.** For a
   user-space stream `php_stream_cast()` calls the userland `stream_cast()`
   method (`main/streams/userspace.c`, `php_userstreamop_cast()`), and such a
   stream reaches this code whenever that method hands back a real socket. From
   inside a `ZEND_HASH_FOREACH_PTR` over `fw.watchers`, userland could then call
   `fpmng_worker_event_free()` — freeing the watcher still held in the loop
   variable, so `event_active()` ran on freed memory — or
   `fpmng_worker_event_create()`, reallocating the table under the iterator.
   Rewritten to snapshot the ids and re-look-up the watcher (and re-fetch its
   stream) after every call that might have re-entered PHP.
   `fw.running = true` also moved to *before* the walk, so that userland hits
   the reentrancy guard instead of reaching a nested `event_base_loop()`.
3. **The busy-spin detector measured the wrong side of the refill.** It
   compared two post-refill sizes, and on a stream with steady inbound TLS the
   refill puts back exactly what was just drained
   (`ext/openssl/xp_ssl.c` refills only when the buffer is empty). So the
   best-behaved consumer — one that drains fully every iteration — would have
   been reported as "nothing consuming them", while a genuinely stuck consumer
   whose buffer merely oscillated would have reset the counter and spun
   silently: both halves of the guard backwards. It now samples the buffer
   *before* the refill, which is the only place the consumer's own progress
   shows.
4. **`fpmng_worker_stream_has_buffered()` returned a value with an exception
   pending.** `Z_PARAM_RESOURCE` accepts any resource, so a curl handle threw
   inside `php_stream_from_zval_no_verify()` and the next line still ran
   `RETURN_BOOL`. Now `RETURN_THROWS()`, as everywhere else in this file.
5. **The spin baseline was not reset by `fpmng_worker_event_disable()`**, so a
   watcher disabled mid-spin and re-enabled later resumed against a stale size.
6. **The documentation claimed the opposite of the code about filtered
   streams** — see above; the paragraph was inverted rather than reworded, and
   `examples/http-direct-worker-react/README.md` said `/strand` still reports
   the stranding while the harness in the same commit turns that into a
   failure. Both corrected.

Not fixed, because checked and correct: `event_active()` cannot re-enter PHP
(libevent only queues; dispatch happens inside `event_base_loop()`); the
`EV_READ`/`event_pending` gating; the shutdown-time `event_base_loop()`, which
needs no pass because `fw.watchers` is destroyed before it; and the `int fd`
versus `php_socket_t` mismatch, which is Windows-only and matches the
pre-existing pattern in `fpmng_worker_event_create()`.

The review also re-confirmed four `findings.md` entries as task-worthy; they
had been confirmed once before under task 073 and still had no task file, so
they now do (082–085). The 64 KB stack buffer stays rejected as a standalone
task for the reason already recorded: the constant is load-bearing for the
bounds check beside it and should move with the extraction in 082.

### Not measured

- Buffering on the **write** side, which the task put out of scope.
- A worker with enough watchers for the per-iteration walk to matter; see the
  cost caveat above.
