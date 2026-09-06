# 041 — TLS: ALPN and SNI — decide the scope before ACME needs them

**Priority:** medium. A decision task with a small implementation attached.
**Status:** open.

## Context

`fpm_http_tls_ctx_new()` (`sapi/fpmng/fpm/fpm_http_tls.c:236-283`) sets a
minimum protocol version, a session id context, `SSL_OP_NO_COMPRESSION` and the
shared ticket key. It sets **no ALPN callback and no SNI callback**, so:

- the server negotiates no application protocol; clients that require an ALPN
  answer fall back to HTTP/1.1 by assumption rather than by agreement
- one pool serves exactly one certificate, whatever the client asked for

Both gaps become concrete in 020: TLS-ALPN-01 is the ACME challenge type that
avoids needing port 80, and it works by answering the `acme-tls/1` protocol
with a throwaway certificate chosen per SNI name.

## Problem

Two separate questions; answer both, implement what the answers require.

1. **ALPN.** Advertise `http/1.1` explicitly? The gateway speaks HTTP/1.1 and
   nothing else (`docs/NOTES.md`, "Czego NIE robimy": no HTTP/2). This is small
   and mostly about being correct on the wire.
2. **SNI.** Does one pool ever serve more than one certificate? Today
   `http.tls_cert` is a single pair. Multiple names on one VPS is a plausible
   small-project shape, and the answer decides whether ACME manages one
   certificate or a set.

Answering "no" to SNI is a legitimate outcome, provided it is written down with
what it costs — including which ACME challenge types stay available.

## Acceptance criteria

1. Both answers recorded in this file, then in `docs/NOTES.md`, before any
   implementation.
2. If ALPN is added: `openssl s_client -alpn http/1.1` reports
   `ALPN protocol: http/1.1`, and a client offering only an unsupported
   protocol is rejected at the TLS layer rather than served HTTP/1.1 anyway.
3. If SNI is added: two names with different certificates on one listener each
   receive their own certificate, verified with
   `openssl s_client -servername`. A connection with no SNI receives a
   documented default rather than a handshake failure.
4. Whatever is added is per-process state built in `fpm_http_tls_ctx_new()`,
   not new fields of the fork-copied `struct fpm_http_tls_s`.

## Notes

- If SNI is answered "no", 020's challenge choice narrows to HTTP-01, which
  forces the port-80 question in 042. The two tasks are coupled; do not settle
  them independently.
