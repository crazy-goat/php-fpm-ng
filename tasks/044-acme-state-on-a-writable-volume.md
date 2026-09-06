# 044 — ACME: define what state lives where, and who owns it

**Priority:** high. Shared prerequisite with the self-runner; `docs/NOTES.md`
section 3l says to settle it once, for both.
**Status:** open. Design, then a small implementation.

## Context

Certificates and the ACME account key are **state, not code**
(`docs/NOTES.md`, 3l, "DWIE RZECZY DO ZAPROJEKTOWANIA TERAZ"). The premise of
the project is an image containing one binary plus application code — immutable.
So the split "immutable code / mutable state on a volume" has to be defined
explicitly, and it is the same split the self-runner needs (section 3a).

This collides with task 010: the gateway does not currently drop privileges, so
today everything it writes is written as whoever started the master.

## Problem

Define the on-disk layout, the ownership and the permissions for ACME state,
and make the gateway (or the ACME cron pool, per 043) create and use it.

## Questions to answer

- What exactly is state: account key, account URL, certificate, private key,
  chain, renewal metadata, and any replay-nonce or order cache.
- One directory or several, and configured by which directive.
- Which uid writes it, given task 010, and what happens when the volume is
  read-only or missing at startup.
- What survives a container restart and what may be regenerated. An account key
  that is regenerated on every boot will hit Let's Encrypt rate limits.

## Acceptance criteria

1. The layout is documented before implementation, in `docs/NOTES.md`, together
   with the self-runner's use of the same split — one answer, not two.
2. The private key and the account key are created with restrictive permissions
   (owner read/write only), verified with `stat` in a test.
3. Neither key ever appears in a log line, at any log level.
4. A missing state directory produces one clear error naming the path, not a
   crash and not a silent fallback to plain HTTP.
5. A read-only state directory is detected at startup, not at renewal time
   ninety days later.
6. Restarting the container reuses the existing account key and certificate; no
   new ACME account is registered. Verified by comparing the account URL across
   restarts.
