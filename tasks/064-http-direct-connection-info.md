# 064 — `fpm_connection_info()` and client certificate exposure in PHP

Status: open
Depends on: 057

## Why

Through FastCGI, PHP only sees what an intermediary chose to pass as request
variables. In HTTP-direct the worker owns the real connection, so the SAPI can
report facts no proxy layer can vouch for: TLS peer certificate details, ALPN
negotiation, connection age, request count per connection. This is a capability
the `http` gateway cannot offer honestly, because its PHP execution is one
process away from the socket.

## Scope

A PHP-callable API (working name `fpm_connection_info()`, final name settled
during implementation) returning connection-level facts from the worker's
bufferevent: transport (plain/TLS), peer address/port, TLS protocol and cipher,
client certificate fields when client verification is enabled, SNI/ALPN values,
connection age, and requests served on that connection so far. Client
certificate verification configuration at the pool level is included, since the
API is meaningless without it.

## Acceptance criteria

- Data-asserting tests over TLS: verified client cert fields visible in PHP,
  unverified/handshake-rejected client behavior, plain-HTTP pool returns
  transport facts without TLS fields.
- Values match the actual connection (cross-checked against an external
  observation, e.g. openssl s_client output), not defaults.
- The API is read-only, available only in direct pools (other pool types get a
  defined "unsupported" answer, tested), and leaks nothing across requests
  (task 054 isolation tests extended).
- Documentation states what is exposed, what is deliberately not (e.g. raw
  socket handles), and the trust model (worker-reported facts are authoritative
  because there is no intermediary).

## Out of scope

- mTLS-based authorization policy; PHP code decides what to do with the facts.
- Any change to how `http` or `fastcgi` pools populate `$_SERVER`.
