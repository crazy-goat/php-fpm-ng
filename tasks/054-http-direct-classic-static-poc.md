# 054 — Direct HTTP classic/static proof of concept

Status: in progress

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

## Workflow checklist

- [x] Task read; acceptance criteria understood
- [x] Isolated worktree and branch
- [x] Implementation and tests; English throughout
- [x] Poligon build/tests using own directory and ports
- [x] Findings recorded if needed (task 055 fairness follow-up)
- [x] Major-issues-only Bugbot review (2 P1 findings fixed, re-verified)
- [ ] Outcome and move to tasks/done
- [ ] PR, green CI, merge
- [ ] Poligon/worktree cleanup; primary checkout updated
