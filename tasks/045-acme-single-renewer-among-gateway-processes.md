# 045 — ACME: exactly one process renews, and the others pick the result up

**Priority:** high. Named in 020 as one of the four decisions blocking ACME.
**Status:** open.

## Context

A `pool.type = http` pool runs `http.gateways` processes (default 2), each with
its own listening socket under `SO_REUSEPORT` and its own `SSL_CTX` built after
`fork()` (`sapi/fpmng/fpm/fpm_http_tls.h`, header comment). Nothing today
elects one of them for anything.

If every gateway process renews independently: N concurrent ACME orders for the
same name, N account registrations or N racing writes to the same key file, and
Let's Encrypt rate limits burned for real.

If option B (a `cron` pool) wins in 043, the renewer is a separate process by
construction and the election question mostly disappears — but the handover
question does not.

## Problem

Ensure exactly one renewer exists, and get the new certificate into every
process that terminates TLS.

## Acceptance criteria

1. With `http.gateways = 4`, a renewal produces exactly **one** ACME order.
   Verified against the staging endpoint by counting orders, or against a local
   test CA (pebble) by reading its log — not by inference from the absence of
   errors.
2. Killing the elected renewer mid-renewal leaves the pool able to renew again
   without an operator action. State the recovery time.
3. After a successful renewal every gateway process serves the new certificate.
   This is 040's mechanism; this task must not grow a second one.
4. No renewal happens twice in a row because two processes each thought they
   were the writer. Verified by running the renewal trigger under all
   `http.gateways` values used in the tests.
5. The election mechanism is per-pool-type behaviour expressed as a field or a
   callback in `fpm_pool_type_s`, never `if (type == ...)` — see `CLAUDE.md`.

## Notes

- The existing shared-memory budget for gateway connections (`docs/NOTES.md`,
  commit "share the connection budget through shared memory") is a precedent
  for cross-process state in this SAPI; look at it before inventing a second
  mechanism.
