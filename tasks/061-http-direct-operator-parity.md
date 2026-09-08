# 061 — Operator parity for HTTP-direct: ping/status, access log, allowed clients, chroot

Status: open
Depends on: —

## Why

The task 054 POC deliberately rejects several operator-facing options during
validation to keep the POC honest: ping/status paths, access log,
`listen.allowed_clients`, and chroot. Each is small, independent, and already
has a defined behavior in other pool types, so bundling them into one task
avoids fifteen-line PRs while keeping each piece separately testable. Status
already works through a separate pool (verified by the task 054 session-status
test), but a pool should be able to report on itself.

## Scope

Implement, per piece, the same observable behavior other pool types provide,
adapted to the direct transport:

- ping and status endpoints handled by the worker's event loop before any PHP
  request, with the scoreboard counting PHP requests only;
- access logging from the worker with the documented format and a validated
  configuration;
- `listen.allowed_clients` enforced at the connection level in the worker;
- chroot support consistent with how the master already prepares workers.

## Acceptance criteria

- Each of the four pieces has data-asserting tests; unsupported combinations
  still fail validation rather than being silently ignored.
- Ping/status answers do not appear as PHP requests in the scoreboard, and
  status of a direct pool reports completed requests, active connections, and
  rejections distinctly.
- Access log records the documented fields for success, rejection (bounded
  buffers), and bodyless responses without corrupting framing.
- `listen.allowed_clients` rejects a disallowed client with tested behavior and
  allows listed ones; chroot plus front-controller validation from task 054
  still hold.
- No behavior change for `http` and `fastcgi` pools (existing suites green).

## Out of scope

- Slowlog, process dump, and full `pm.status_path` JSON extensions; follow-ups
  if measurements or operators ask for them.
- Anything from tasks 057-060 and 063+.
