# 047 — ACME: issuance and renewal end to end, staging by default

**Priority:** high — this is the actual feature; everything before it is
groundwork.
**Status:** open. Depends on 043, 044, 045, and on 040 for installing the
result.

## Context

RFC 8555 is a protocol, not a call: account key, JWS signing, nonce handling,
`order → authz → challenge → finalize → cert`, CSR generation. Per 020, the
gateway process has neither an HTTP client nor a JSON parser today; whether it
needs one at all depends on 043.

Let's Encrypt rate limits are a practical hazard, not a theoretical one — a
test loop against production can block the domain for a week.

## Problem

Obtain a certificate for a configured name and renew it before expiry,
unattended.

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

## Explicitly out of scope

- Certificate types other than what the chosen challenge supports; DNS-01;
  multiple CAs; OCSP stapling. Each is a separate task if it is ever wanted.
