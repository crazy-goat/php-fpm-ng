# 081 — The worker executor has no way to log

Status: open
Type: decision + implementation
Depends on: 080 (merged)
Related: `examples/http-direct-worker/FpmngServer.php`,
`examples/http-direct-worker-react/FpmngReactServer.php`,
`sapi/fpmng/tests/fpmng-http-direct-worker-drain.phpt`

## Why

`STDERR` is **not defined** in this SAPI. The constant is registered by the CLI
SAPI; FPM does not register it. So `fwrite(STDERR, ...)` in a worker script
throws `Error: Undefined constant "STDERR"`.

Both example bridges report handler failures with exactly that call, and both
do it from inside an exception handler:

- `examples/http-direct-worker/FpmngServer.php` — in the `catch (\Throwable)`
  inside `Amp\async()`. The `Error` escapes the fiber into Revolt's
  uncaught-throwable handler, which by default rethrows out of
  `EventLoop::run()`: **one failing request handler kills the worker** instead
  of logging a line.
- `examples/http-direct-worker-react/FpmngReactServer.php` — the same call in a
  promise rejection path.

Both READMEs state the opposite of the truth: that the write "goes to stderr,
i.e. the FPM error log with `catch_workers_output = yes`".

Measured while writing the test for task 080: a `/probe` handler wrote nothing
at all, and `defined("STDERR")` came back `false`. The test works around it by
writing to a file, with a comment saying why.

## Scope

1. **Decide the supported logging path** for `pool.executor = worker` and write
   the decision down before coding. Two shapes:
   - Register the standard stream constants (`STDIN`/`STDOUT`/`STDERR`) for
     this executor, so the ordinary PHP idiom works and `catch_workers_output`
     picks it up. Closest to what an author expects; needs a look at why FPM
     does not define them and whether anything depends on that.
   - Add an `fpmng_worker_log()` builtin going straight to `zlog()`, so the
     line lands in the pool's error log with the pool name and severity that
     every other line has. Explicit, but it is a second idiom to learn.
2. Fix both bridges to use it.
3. Fix both READMEs, which currently document behaviour that does not exist.

## Acceptance criteria

- A worker script can report a handler failure and the line reaches the FPM
  error log; asserted by a `.phpt` with `expectLogPattern()`.
- A handler that throws does **not** take the worker down. Regression test for
  the amphp bridge's fiber path specifically, which is where the current code
  turns a logged error into a dead worker.
- Both READMEs describe what actually happens.
- `sapi/fpmng/tests/fpmng-http-direct-worker-drain.phpt`'s `/probe` route drops
  its file-based workaround and its comment.

## Out of scope

- Logging from the classic (non-worker) `http-direct` executor.
- Anything about `catch_workers_output` itself.
