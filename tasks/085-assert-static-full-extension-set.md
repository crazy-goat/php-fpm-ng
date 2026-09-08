# 085 — Assert `build/static-full.sh`'s extension set against the artefact

Status: open
Type: build + test
Related: `build/static-full.sh`

## Why

The extension set `static-full.sh` configures is asserted nowhere, so dropping
one is silent until an example breaks at runtime — which already happened.
Task 074 had to add `--enable-filter --enable-ctype` after `amphp/socket`'s
`connect()` failed with `Error: Invalid URI: tcp://mysql:3306`:
`league/uri-interfaces` calls `filter_var($host, FILTER_VALIDATE_IP)` in
`UriString.php:711`, and `--disable-all` had omitted both. The failure surfaced
three layers away from its cause, in a userland library, as a URI error.

Re-confirmed as task-worthy by the task 079 review, and left in the gitignored
`findings.md` without a task file since 2026-09-08.

## Scope

Assert against the **produced binary, not the configure flags** — that
distinction is the whole point, because the flags are exactly what was already
wrong and a check that reads them would have passed. `strings` (or a
`php -m` / one-liner run of the artefact, which is more direct now that the
binary is a working static-pie build) for the functions the examples actually
depend on: `filter_var`, `ctype_digit`, `json_encode`, `openssl_encrypt`, and
the SAPI's own `fpmng_worker_*` builtins.

Keep the list short and tied to a named consumer: an assertion nobody can
explain gets deleted the first time it is inconvenient.

## Acceptance criteria

- Removing `--enable-filter` from `static-full.sh` makes the script fail with a
  message naming the missing function, demonstrated once.
- The check runs as part of `static-full.sh` itself, so anyone building the
  artefact gets it, not only CI.
- Each asserted symbol has a comment naming what needs it and why.

## Out of scope

- Widening the extension set; this is about noticing changes to it.
- `build/dynamic.sh`, which builds upstream's `sapi/fpm` for comparison.
