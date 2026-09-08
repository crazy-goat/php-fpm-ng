# 065 — Full response control: early hints and custom methods in HTTP-direct

Status: open
Depends on: —

## Why

Intermediaries filter what an application can say: nginx and the `http` gateway
constrain which statuses, methods, and header semantics pass through. In
HTTP-direct the PHP script *is* the server, so the transport can expose
protocol features that were previously unreachable from PHP, starting with
103 Early Hints, custom response statuses, and arbitrary (non-standard)
request methods — cheap to support once, useful to real applications.

## Scope

Protocol-level passthrough in the direct worker: emitting 103 Early Hints
before the final response, accepting and reporting arbitrary request methods
to PHP (with a documented safety list/allow-list decision), and allowing
status codes the SAPI might currently normalize or reject. Each capability is
opt-in or validated where it carries risk (arbitrary methods), decided and
documented during implementation.

## Acceptance criteria

- Data-asserting test: a script emits 103 with headers, then a final response;
  a raw-socket client verifies the exact wire order and that framing stays
  intact.
- Custom method (e.g. `REPORT`) reaches PHP with the exact name in
  `$_SERVER['REQUEST_METHOD']`; the allow-list decision is validated at config
  time and tested.
- Early-hints plus bodyless responses and HEAD (task 054 framing rules) are
  tested together for keep-alive correctness.
- Unsupported combinations fail validation; documentation in
  `docs/http-direct.md` states what is exposed and the security reasoning for
  any method restrictions.

## Out of scope

- HTTP/2 push semantics (different protocol, task 071).
- Changing nginx or the `http` gateway; they keep their current filtering.
