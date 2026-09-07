# 047 — ACME: issuance and renewal end to end, staging by default

**Priority:** high — this is the actual feature; everything before it is
groundwork.
**Status:** open. Depends on 043, 044, 045, and on 040 for installing the
result.

## Context

RFC 8555 is a protocol, not a call: account key, JWS signing, nonce handling,
`order → authz → challenge → finalize → cert`, CSR generation. Task 043 chose a
project-owned PHP client embedded in the binary and run by a dedicated `cron`
pool. The client requires the PHP OpenSSL extension and a documented HTTPS
client mechanism; it must not depend on the user's application or Composer
dependencies.

Let's Encrypt rate limits are a practical hazard, not a theoretical one — a
test loop against production can block the domain for a week.

## Problem

Obtain a certificate for a configured name and renew it before expiry,
unattended, using the embedded PHP client and the bootstrap state machine from
`docs/NOTES.md` section 3l.

## Acceptance criteria

1. **The staging directory is the default.** Production requires an explicit
   opt-in in the configuration. A misconfigured test must not be able to burn
   production rate limits.
2. Issuance works end to end against Let's Encrypt staging, or a local pebble,
   for a fresh name: account registered, order finalized, certificate written to
   the state directory from 044, listener serving it.
3. Renewal is triggered by remaining lifetime, not by a fixed calendar, and the
   threshold is stated. A certificate that is already valid is not reissued on
   every boot — verified by restarting the pool three times and counting orders.
4. Renewal completes without dropping connections, with no operator action.
   Verified with a load generator running across the renewal: zero connection
   errors.
5. **Failure is loud and safe.** A failed renewal logs an error naming the
   failure, keeps the existing certificate serving, and retries with backoff.
   It never leaves the listener down and never falls back to plain HTTP.
   Verified by pointing the client at an unreachable directory URL mid-life.
6. A certificate that expires despite retries is visible before it expires:
   state what the operator sees, and when.
7. No log line at any level contains the account key, the private key, or a
   key authorization value.
8. A build advertised as ACME-capable validates the required PHP OpenSSL
   extension and the chosen HTTPS client capability at startup. A missing
   requirement produces one clear error before an order is attempted.
9. The project-owned ACME script is read from its distinct embedded
   distribution payload (append-only payload plus typed footer from section
   3l), not from the user's application payload or filesystem. Repacking an
   application leaves the embedded ACME script unchanged.
10. Fresh bootstrap follows `NO_CERT → ISSUING → READY`: port 443 remains
    closed until atomic certificate installation, while port 80 answers only
    active HTTP-01 challenges and otherwise does not forward to a worker.

## Explicitly out of scope

- Certificate types other than what the chosen challenge supports; DNS-01;
  multiple CAs; OCSP stapling. Each is a separate task if it is ever wanted.
