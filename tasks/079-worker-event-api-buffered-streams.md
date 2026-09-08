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

## Acceptance criteria

- The decision between (1) and (2) is written down with its reason, including
  what it would break, before any code changes.
- A regression test that strands a buffered TLS stream and asserts the chosen
  behaviour. `examples/http-direct-worker-react/`'s `/strand` route plus
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
