# 079 — Worker event API: streams whose data is invisible to libevent

Status: open
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
