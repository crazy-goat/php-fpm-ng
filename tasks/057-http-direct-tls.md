# 057 — TLS for HTTP-direct pools

Status: open
Depends on: —

## Why

`pool.type = http-direct` (task 054) currently rejects TLS configuration during
validation: the POC accepts plain HTTP only (`docs/http-direct.md`, "Deliberate
limits"). The classic `http` gateway already serves TLS and reloads certificates
without restart (task 040), so plain-HTTP-only is the largest operator-facing gap
of the direct transport. Serving TLS inside the worker is architecturally
simpler than in the gateway: each worker owns its own bufferevents and there is
no separate process to coordinate.

## Scope

TLS termination inside the HTTP-direct worker using libevent's OpenSSL support
(bufferevent_openssl), driven by pool configuration: certificate/key paths,
protocol/cipher options equivalent to what the `http` gateway exposes, and
certificate reload without restarting workers, mirroring the task 040 reload
semantics (in-flight connections finish on the old context).

## Acceptance criteria

- A direct pool serves HTTPS on `listen` with a valid certificate; plain HTTP on
  the same pool is refused or documented, not silently accepted.
- Certificate reload (task 040 test shape) swaps the served certificate with no
  restart; in-flight requests complete on the old certificate.
- Data-asserting tests cover: TLS handshake, request over TLS, reload, rejected
  handshake (wrong client SNI or untrusted CA where applicable), and that
  non-TLS configuration still behaves exactly as today.
- Configuration that is unsupported must fail validation rather than pretend,
  keeping the task 054 rule.
- Documented statement of what is not covered (e.g. client-certificate
  verification may land in task 064 instead) in `docs/http-direct.md`.

## Out of scope

- Static files (task 060), streaming (task 058), HTTP/2 (task 071).
- The `http` and `fastcgi` pool types; their TLS behavior stays unchanged.
