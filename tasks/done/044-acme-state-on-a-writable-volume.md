# 044 — ACME: define what state lives where, and who owns it

**Priority:** high. Shared prerequisite with the self-runner; `docs/NOTES.md`
section 3l says to settle it once, for both.
**Status:** done (2026-09-07) — see Outcome below.

## Context

Certificates and the ACME account key are **state, not code**
(`docs/NOTES.md`, 3l, "TWO THINGS TO DESIGN NOW"). The premise of
the project is an image containing one binary plus application code — immutable.
So the split "immutable code / mutable state on a volume" has to be defined
explicitly, and it is the same split the self-runner needs (section 3a).

This collides with task 010: the gateway does not currently drop privileges, so
today everything it writes is written as whoever started the master.

## Problem

Define the on-disk layout, ownership and permissions for ACME state, and make
the embedded PHP ACME client in its dedicated `cron` pool (the decision in
043) create and use it.

## Questions to answer

- What exactly is state: account key, account URL, certificate, private key,
  chain, renewal metadata, and any replay-nonce or order cache.
- One directory or several, and configured by which directive.
- Which uid the dedicated ACME cron process uses to write it, given task 010,
  and what happens when the volume is read-only or missing at startup.
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
7. The PHP ACME process is the only writer. Gateway processes receive challenge
   state and certificate-install notifications without writing account or
   certificate state themselves.

## Outcome — 2026-09-07

Layout, ownership and permissions are documented in `docs/NOTES.md` section
3y: one base directory (`env[ACME_STATE_DIR]` on the ACME `cron` pool — the
existing FPM `env[]` directive, no new C code), one subdirectory per domain
underneath it, `account.key`/`account.json`/`<domain>/privkey.pem` at 0600,
`<domain>/fullchain.pem` at 0644. No separate directive was needed for "which
directory": everything lives under the one base directory task 044 asked to
settle.

Implemented as `sapi/fpmng/acme/state.php`, a small library the (not yet
written — task 047) ACME cron script will use:
`State::fromEnvironment()`/`assertUsable()` fail with one clear message
naming the path for a missing or read-only state directory, before any key
material is touched; `loadOrCreateAccountKey()`/`loadOrCreateCertKey()`
generate an EC P-256 key once and reuse it thereafter;
`loadOrRegisterAccount(callable $register)` persists the registration
result and never calls `$register` again once `account.json` exists;
`installCertificateChain()` writes the public chain. All writes go through a
temp file in the same directory, `chmod`ed to its final mode, then
`rename()`d, so a concurrent reader never sees a partial or wrongly-permissioned
file.

Verified by `sapi/fpmng/tests/acme-state.phpt`, run directly against the
local PHP 8.5.10 CLI (`php sapi/fpmng/tests/acme-state.phpt`'s `--FILE--`
section) before commit, and by CI's `phpt` job thereafter: a missing
directory and an 0500 (non-writable) directory each throw one message naming
the path; `account.key`, `<domain>/privkey.pem` and `account.json` are
verified 0600 and `<domain>/fullchain.pem` 0644 via `fileperms()`; a second
`State` instance over the same directory (simulating a restart) reuses the
identical account key bytes, certificate key bytes and account URL, and its
registration callback is never invoked; a corrupted key file produces an
error that names the path but never contains `BEGIN`/`PRIVATE` (criterion 3).

Criterion 7 (sole writer) holds by construction, not by an added check: the
HTTP gateway has no code path that writes under this directory today — see
`docs/NOTES.md` 3y.

Left out, by design, for later tasks: the actual RFC 8555 protocol (045's
handover between cron and gateway processes, 047's issuance/renewal) is not
part of this task. `state.php` only defines and manages the on-disk layout;
`installCertificateChain()` is exercised in the test with a placeholder PEM
string, not a certificate obtained from a CA.
