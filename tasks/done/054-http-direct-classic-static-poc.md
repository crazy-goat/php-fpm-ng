# 054 — Direct HTTP classic/static proof of concept

Status: done

## Why

The HTTP gateway serializes requests to FastCGI and forwards them to separate
PHP workers (`sapi/fpmng/fpm/fpm_http.c:1-23`). The in-process alternative was
previously deferred, not prohibited (`docs/NOTES.md`, section 3g). The user now
requests an isolated experiment without FastCGI, fibers, or a new process manager.

## Scope

An opt-in `pool.type = http-direct` with the classic PHP request lifecycle and
`pm = static` only. Existing HTTP/FastCGI pools retain their behavior. The FPM
master still owns worker creation, supervision, and replacement.

This POC executes one explicitly configured PHP front controller for all paths;
it is not a replacement for the gateway's static-file, TLS, proxy, or routing
features. Responses may be buffered, with a documented bound. Streaming,
`fastcgi_finish_request`, dynamic/ondemand PM, fibers, HTTP/2, TLS, and performance
claims without measurement are out of scope. Before the PR, benchmark on the
poligon: nginx + FastCGI versus the built-in HTTP gateway versus HTTP-direct,
with the same scripts, static worker count, and HTTP client workload. A raw
FastCGI client is not a substitute for the nginx baseline. Unsupported configuration must fail rather than pretend
to work.

## Acceptance criteria

- A static pool serves HTTP directly on `listen`, without gateway children or an
  internal FastCGI listener/connection. Explicit `pool.executor = classic` works;
  fiber/async and dynamic/ondemand configurations fail validation.
- GET/query, POST form/raw body, cookies, request headers, response status and
  repeated headers, HEAD, and consecutive requests have data-asserting tests.
- Each PHP request has its own startup/shutdown, including extension lifecycle,
  shutdown functions, and reset application globals/classes.
- Multiple children serve concurrent requests; `pm.max_requests` replacement,
  worker crash replacement, master request timeout, and graceful reload/stop
  are tested. The scoreboard reports completed PHP requests.
- Request/header and response buffers are bounded. Unsupported gateway options
  fail validation; client-selected paths never select arbitrary scripts.
- Relevant existing suites pass. Tests run in CI. Documentation states the
  limitations and measured/not-measured results.

## Outcome

Implemented `pool.type = http-direct` (POC): libevent HTTP parsing and classic
PHP request execution inside each FPM child; `pm = static` and
`pool.executor = classic` only, enforced by validation (fiber/async,
dynamic/ondemand, and unsupported gateway options fail configuration testing).
The FPM master keeps creating, supervising, recycling (`pm.max_requests`),
replacing crashed children, and enforcing `request_terminate_timeout`; graceful
reload/stop drain in-flight requests. Routing is a fixed, validated front
controller inside `chdir`; client paths, `doc_root`, and `user_dir` cannot
select another script (review finding). Responses are buffered (8 MiB body,
64 KiB headers) and bodyless 204/205/304/HEAD responses are drained to keep
keep-alive framing correct (review finding). Five new `.phpt` files cover
protocol, config rejection, lifecycle, session/scoreboard, and routing/framing.

Poligon (PHP 8.5.9, 4 static workers, median of 3 runs, see
`docs/http-direct.md`): http-direct achieved the highest successful throughput
and lowest CPU per response in all four scenarios — tiny-c1 8.4k vs 5.7k
(nginx+FastCGI) and 7.1k (`http`) rps; cpu-c32 4.4k vs 3.7k and 2.2k rps; worst
p99 at cpu-c32 (16 ms) consistent with the documented single-event-loop and
accept-fairness limits. The `http` gateway's high raw request rate at
concurrency 32 is mostly 503 rejections (714k/813k non-2xx). Full fpmng suite:
14 PASS, 4 SKIP (fiber build flag), 0 FAIL.

Left out: TLS, static files, streaming, `.user.ini` handling, total connection
limit, HTTP/2, fairness improvements (task 055).

- [x] Task read; acceptance criteria understood
- [x] Isolated worktree and branch
- [x] Implementation and tests; English throughout
- [x] Poligon build/tests using own directory and ports
- [x] Findings recorded if needed (task 055 fairness follow-up)
- [x] Major-issues-only Bugbot review (2 P1 findings fixed, re-verified)
- [ ] PR, green CI, merge
- [ ] Poligon/worktree cleanup; primary checkout updated
