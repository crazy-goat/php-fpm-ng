# 043 — ACME: decide where the client lives, C in the gateway or PHP in a cron pool

**Priority:** high — it is the first ACME task; every other one depends on the
answer.
**Status:** open. Decision, no implementation.

## Context

`docs/NOTES.md`, section 3l records both options and their trade-offs, and
deliberately leaves the choice open. Summary, not a replacement for reading it:

**Option A — ACME in C, inside the gateway.** Bootstrap is natural (you cannot
serve HTTPS before the first certificate exists, and a `cron` pool starts after
the pools do). No coupling to the user's application. "One binary" stays true.
No dependency on PHP extensions — OpenSSL is linked anyway for TLS. Costs a few
thousand lines of C and a permanent memory-safety surface. The gateway today
has neither an HTTP client nor a JSON parser.

**Option B — ACME in PHP, as a `pool.type = cron`.** A few hundred lines,
libraries exist, reuses machinery already built (cron, static files for the
challenge). Easier to fix without rebuilding the binary. The downsides are
exactly A's advantages inverted.

## What settles it — the three checks from section 3l, unchanged

1. **How much C is it really?** Read a minimal C ACME client (`uacme`,
   `acme-client`) and count what is actually needed for HTTP-01 against a
   single CA. Report the number, not an impression.
2. **Can option B's bootstrap be made sane?** For example: the gateway starts
   without TLS, serves only the challenge, and the HTTPS listener comes up
   after the first issuance. Is that a clean state machine or a pile of special
   cases?
3. **Could the ACME script be embedded in the binary** by the same mechanism as
   the self-runner (section 3a) without coupling the two features?

## Acceptance criteria

1. Each of the three checks answered with evidence: a line count from a real
   codebase, a described state machine, a yes/no with the mechanism named.
2. A decision, written in `docs/NOTES.md` section 3l, replacing "NIEROZSTRZYGNIĘTE".
3. The dependent tasks (044, 045, 046, 047) updated to match the choice —
   several of their acceptance criteria only make sense for one of the two.
4. If the answer turns out to be "neither, mount a certificate instead", that is
   a complete outcome: record it, document the supported path, and close 020.

## Explicitly out of scope

- Writing any ACME code before this file has an answer in it.

## Outcome

The client will be a project-owned PHP script embedded in the binary as a
separate typed append-only payload and run by one dedicated `cron` pool.
`docs/NOTES.md` section 3l records all three checks: a reproducible 8,327-line
physical-source baseline from `uacme` revision
`e9cfa6f052644864a28c7d9d04756900abfc653f`, the
`NO_CERT → ISSUING → READY` bootstrap state machine, and the independent
payload/footer embedding mechanism. Tasks 044-047 now assign writing and
renewal to that PHP process and challenge/certificate handover to gateways.
No ACME implementation was added.
