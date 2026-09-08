# 062 — `.user.ini` per-directory INI for HTTP-direct pools

Status: open
Depends on: —

## Why

The task 054 POC intentionally does not activate the CGI SAPI's per-directory
`.user.ini` hook, documented in `docs/http-direct.md`: with a fixed front
controller the "directory" of the request is not the directory of any executed
script, so naive reuse would apply ini settings based on client-chosen paths.
The hook itself exists and is well understood; the missing work is defining
which directory governs and making that safe.

## Scope

Define and implement `.user.ini` discovery for direct pools based on the
validated front controller's directory (the only script that executes),
evaluated before request startup, with the same caching and mtime semantics as
the CGI SAPI. Client-supplied paths must never influence which ini files are
read. The behavior must be opt-in or explicitly documented as default-on to
match other pool types, decided during implementation and recorded in
`docs/http-direct.md`.

## Acceptance criteria

- A `.user.ini` next to the front controller changes ini values for requests to
  that pool; a control pool without the file is unaffected.
- Data-asserting tests cover: ini applied per request, ini cached across
  requests, mtime-based re-read, ini outside the front controller's directory
  (including client-path lookalikes) never applied, and per-request ini reset
  after shutdown (no leakage between requests, extending the task 054
  isolation tests).
- Traversal-style request paths cannot change the effective ini set; a test
  proves it.
- Unsupported combinations fail validation; the task 054 config-rejection suite
  is updated accordingly and still passes.

## Out of scope

- `.htaccess`-style Apache semantics; only PHP `.user.ini` files.
- INI changes for other pool types.
