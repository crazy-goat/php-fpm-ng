# 030 — Session RINIT runs before the request's superglobals exist

**Track:** nice-to-have. This task only applies to `pool.executor = fiber`,
which is moving behind a build flag that is **off by default**, so a stock
binary does not contain this code at all. The `Priority:` line below is the
priority *within* the fiber track — it is not a claim against the HTTP,
cron, scheduler or proxy work, which is where the project is focused.

**Priority:** medium. One symptom is already contained by a startup refusal;
the underlying ordering is not fixed and may affect more than sessions.
**Status:** open.

## Context

In `sapi/fpmng/fpm/fpm_pool_coop.c` the per-request sequence calls
`fpm_coop_session_request_startup()` (ext/session's RINIT for this request)
**before** `zend_activate_auto_globals()` builds `$_GET`, `$_POST`, `$_COOKIE`
and `$_FILES` from the current `SG`. In the classic SAPI the equivalent work
happens the other way round, inside `php_request_startup()` →
`php_hash_environment()`.

Anything that reads a request superglobal from an extension's RINIT therefore
sees the wrong data: not stale from the previous request in the usual sense,
but the container's, because the request's own arrays have not been built yet.

## The known symptom

`session.auto_start = 1` can never pick up the request's `PHPSESSID`, because
`php_rinit_session()` starts the session itself, synchronously, at a point
where `$_COOKIE` does not yet hold this request's cookies. Measured: two
sequential requests carrying an identical cookie receive two different,
freshly generated session ids. The RED suite covers this (test 10, superseded
in practice by test 12 once the refusal landed).

That symptom is now contained — `session.auto_start = 1` is refused at pool
startup under `pool.executor = fiber`, see the merge of
`feature/session-lock-field-patch` and `docs/session-lock-arbiter-report.md`.
Containment is not a fix: the refusal hides the one case we found, it does not
correct the order.

## Why this is worth its own task

`ext/session` is not special here. Any extension whose RINIT touches
`PG(http_globals)` has the same problem in coop pools, and would show it as
wrong behaviour rather than as an error — the arrays exist, they simply belong
to the container. We have not surveyed which loaded extensions do this.

The reason the current order exists needs to be established before changing it.
`fpm_coop_session_request_startup()` may have been placed first deliberately,
and the auto-globals block immediately after it carries its own reasoning about
`auto_globals_jit` being frozen at `php_module_startup()`. Whoever picks this up
should understand both before moving either.

## Acceptance criteria

- The reason for the current ordering is written down, or established as
  accidental.
- Either the order is corrected so that a per-request RINIT sees this request's
  superglobals, or it is documented as a permanent limitation of coop pools with
  the list of extensions it affects.
- If the order is corrected: `session.auto_start` either works (two requests
  with the same cookie share a session id) or is still refused for the
  locking reason alone, with the message updated to drop the cookie argument.
- A survey of the RINIT functions of the extensions we ship or expect, naming
  which ones read request superglobals.

## Out of scope

The session locking work itself, which is merged and independent of this.
