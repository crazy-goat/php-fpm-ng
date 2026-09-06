# 020 — ACME: obtain and renew TLS certificates in the gateway

**Priority:** low. Deliberately deferred; large, and partly replaceable by
existing tooling.
**Status:** open, not started by decision. Depends on TLS termination, which is
done (`sapi/fpmng/fpm/fpm_http_tls.c`).

## Context

The project's premise is one binary plus application code in a container image:
no nginx, no supervisord, no system cron, no shell. Certificates are the
remaining piece that normally requires another process.

TLS termination with a static certificate now exists — `http.tls_cert`,
`http.tls_key`, a shared session-ticket key across gateway processes. ACME was
scoped out at that point on purpose.

## Why it was deferred

RFC 8555 is a protocol, not a call: account key, JWS signing, nonce handling,
`order → authz → challenge → finalize → cert`, CSR generation. The gateway
process today has no HTTP client and no JSON parser. On top of that, several
decisions are the project owner's, not an implementer's:

- **HTTP-01 needs port 80** for `/.well-known/acme-challenge/<token>`, while the
  pool listens wherever it is told. TLS-ALPN-01 avoids port 80 but requires
  certificate switching by ALPN.
- **Which of N gateway processes renews?** `http.gateways` defaults to 2, with
  `SO_REUSEPORT`. This needs single-writer election and a way to hand the new
  certificate to the others.
- **Where does the private key live, and who owns it?** See task 010 — the
  gateway does not currently drop privileges.
- **Let's Encrypt rate limits.** Testing must use the staging directory or risk
  blocking the domain. This is a practical hazard, not a theoretical one.

There is also an honest question of value: for "one small VPS, one container", a
certificate mounted as a volume or a secret, renewed by certbot alongside,
covers a large share of cases. ACME in the binary is elegant but it is a week of
work and permanent maintenance surface.

## Problem

Decide whether to build it, and if so, settle the questions above first.

## Acceptance criteria (if it proceeds)

1. The four decisions above are made and written down **before** implementation.
2. Certificate acquisition and renewal work end to end against the Let's Encrypt
   **staging** endpoint, with the staging endpoint as the default so that a
   misconfigured test cannot burn production rate limits.
3. Renewal happens without dropping connections and without an operator action.
4. Exactly one process renews; the others pick up the new certificate. Verified
   with `http.gateways` greater than 1.
5. The private key is written with restrictive permissions and never appears in
   any log. The project has already had one near-miss logging a credential-
   bearing identifier.
6. Failure to renew is loud and leaves the existing certificate in place rather
   than breaking the listener.

## Explicitly out of scope until the decision is made

- Any implementation work at all. This task is a decision first.

## Notes

- If the answer is "no", that is a complete outcome: record it, document
  mounting a certificate as the supported path, and move this file to `done/`.
