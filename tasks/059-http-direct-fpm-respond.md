# 059 — `fpm_respond()`: send the response and keep computing

Status: open
Depends on: 058

## Why

`fastcgi_finish_request()` is disabled in HTTP-direct pools because there is no
FastCGI request to finish; the task 054 POC offers no equivalent. The natural
direct-transport version is stronger than the FastCGI one: once the response is
on the wire, the worker can return to its event loop, serve other connections,
and let the remaining PHP work complete afterwards. That is impossible through
the FastCGI path, where the script keeps blocking its worker until it exits.

## Scope

A PHP-callable function (name to be settled during implementation, working name
`fpm_respond()`) that finishes the current HTTP response — status, headers, and
buffered body — and marks the request as complete for scoreboard and lifecycle
purposes while the PHP script continues. Interaction with the classic executor
must be honest: if the executor is blocking, other connections progress only
between PHP operations, and that must be measured and documented rather than
promised away. Response state after the call must be sealed (later output is
discarded or erroring, never corrupting keep-alive framing).

## Acceptance criteria

- Data-asserting test: a script calls the function, the client receives the
  complete correct response, and code after the call still runs (observable via
  a log/scoreboard side effect).
- Scoreboard counts the request as completed at the right moment and reports
  the worker as serving again; `pm.max_requests` accounting stays correct.
- Second response on the same keep-alive connection after the first was finished
  early is byte-exact (framing regression coverage from task 054 reused).
- Documented, tested behavior when output is attempted after the call, when the
  function is called twice, and when a fatal error occurs after the call.
- Measured effect on concurrent-request latency while a post-response script
  keeps running (task 054 harness shape), including a negative result if the
  blocking executor erases the benefit.

## Out of scope

- Background/queue semantics beyond "finish the current request early"; no
  scheduler and no fibers.
- The `http` gateway and `fastcgi` pools; `fastcgi_finish_request()` there is
  unchanged.
