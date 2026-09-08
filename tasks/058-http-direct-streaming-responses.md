# 058 — Streaming responses for HTTP-direct

Status: open
Depends on: —

## Why

The task 054 POC buffers the entire response body (8 MiB bound) before sending
final status and headers. For large or progressively generated payloads this
costs latency and memory. The worker already owns a libevent event loop, so
flushing parts of the response while the script runs is a natural extension and
is a prerequisite for `fpm_respond()` (task 059) and later server-sent events
(task 070). The classic `http` gateway cannot stream from PHP cheaply because
FastCGI output still passes through the gateway process.

## Scope

Allow the SAPI flush path to push buffered output into the connection's
evbuffer as a chunked transfer-encoding response before script completion.
Define and document the interaction with the blocking classic executor: while
PHP executes, the event loop is not pumping, so writes land in the socket
buffer and backpressure semantics must be explicit (block, drop, or error past
a bound). Keep the buffered path as the default; streaming becomes opt-in.

## Acceptance criteria

- An opt-in configuration (or script-level decision) enables chunked streaming;
  default configuration keeps today's fully-buffered behavior, verified by the
  existing task 054 tests unchanged.
- A data-asserting test streams a body larger than the 8 MiB buffered bound and
  verifies the client receives every byte in order with a valid chunked framing.
- Explicit, documented behavior when the client stalls: bounded socket buffer,
  documented failure mode, no unbounded memory growth in the worker.
- HEAD/204/205/304 framing rules from task 054 still hold (no stray bytes on
  keep-alive).
- Measured latency or memory improvement (or an explicit negative result)
  recorded in the Outcome using the task 054 benchmark harness shape.

## Out of scope

- `fpm_respond()` semantics (task 059) and long-lived connections (task 070).
- Fiber/async execution; this task assumes the classic blocking executor.
