# 045 — ACME: exactly one process renews, and the others pick the result up

**Priority:** high. Named in 020 as one of the four decisions blocking ACME.
**Status:** open.

## Context

A `pool.type = http` pool runs `http.gateways` processes (default 2), each with
its own listening socket under `SO_REUSEPORT` and its own `SSL_CTX` built after
`fork()` (`sapi/fpmng/fpm/fpm_http_tls.h`, header comment).

Task 043 chose one project-owned PHP client in a dedicated `cron` pool. That
process is the sole renewer by construction; gateway election is not part of
this task. The handover problem remains: every gateway must answer the active
challenge and install the resulting certificate.

## Problem

Ensure only one dedicated ACME process can run per configured certificate, and
get its challenge and certificate updates into every gateway process.

## Acceptance criteria

1. With `http.gateways = 4`, a renewal produces exactly **one** ACME order.
   Verified against the staging endpoint by counting orders, or against a local
   test CA (pebble) by reading its log — not by inference from the absence of
   errors.
2. Killing the ACME cron process mid-renewal leaves the pool able to renew again
   without operator action. State the recovery time.
3. After a successful renewal every gateway process serves the new certificate.
   This is 040's mechanism; this task must not grow a second one.
4. No renewal happens twice in a row because a scheduler restart or overlapping
   tick started a second writer. Verify this while varying `http.gateways` and
   while restarting the ACME cron process during one order.
5. ACME scheduling and handover are per-pool-type behaviour expressed as a
   field, callback or data in `fpm_pool_type_s`, never `if (type == ...)` — see
   `CLAUDE.md`.

## Notes

- The existing shared-memory budget for gateway connections (`docs/NOTES.md`,
  commit "share the connection budget through shared memory") is a precedent
  for cross-process state in this SAPI; look at it before inventing a second
  mechanism.
- This task must not add leader election among gateway processes. The dedicated
  ACME cron process owns issuance and renewal.
