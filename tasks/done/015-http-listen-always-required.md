# 015 — `http.listen` is effectively always required, contradicting its own error message

**Priority:** low. Small, self-contained, and user-facing.
**Status:** done. Found while doing something else and deliberately not fixed
then; recorded in `docs/NOTES.md` around line 2515.

## Context

`fpm_conf.c` checks type-specific directives **before** the `/* listen */`
block. As a result `type->validate()` — for the HTTP gateway,
`fpm_http_validate_pool()` — runs while `wp->listen_address_domain` is still
unset (zero: neither `FPM_AF_UNIX` nor `FPM_AF_INET`).

The gateway's check reads "`listen_address_domain != FPM_AF_INET` requires
`http.listen`". Against an unset value that condition is **always true**, so
`http.listen` is required for every `pool.type = http`, including
`listen = 127.0.0.1:9001`, where the documented behaviour is that the gateway
defaults to the FastCGI port + 1.

The failure is in the safe direction — the requirement is too broad, not too
narrow — but the error message and the documentation both say something that is
not true, and every test configuration in this project has had to carry a
redundant `http.listen` because of it.

## Problem

Make the validation see the value it is testing, so the documented default
(FastCGI port + 1 for a TCP pool) actually works.

## Acceptance criteria

1. A pool with `pool.type = http` and `listen = host:port` and **no**
   `http.listen` starts, and its gateway listens on the FastCGI port + 1.
2. A pool with `pool.type = http` listening on a unix socket and no
   `http.listen` is still refused, with the existing message, which is now true.
3. Reordering the checks does not break any other type-specific validation. The
   ordering exists for some reason; establish what depends on it before moving
   anything, and if type validation genuinely must run early, then the fix is on
   the other side — the gateway's check has to be deferred to a point where the
   listen address is known.
4. A test covers both cases (task 003).

## Notes

- Worth checking whether any other `type->validate()` implementation reads pool
  fields that are not yet populated at that point. This may not be the only one.

## Outcome

- Established what the original ordering (type-specific checks, i.e.
  `type->validate()`, before the `/* listen */` block) actually depends on:
  `fpm_pool_status_validate()` and `fpm_pool_supervisor_validate()`
  (`fpm_pool_status.c:90`, `fpm_pool_supervisor.c:163-164`) both set
  `wp->config->pm`/`pm_max_children`, which the "pm" validation further down
  `fpm_conf_process_all_pools()` (`fpm_conf.c`, around what was line 1008)
  depends on. That dependency only requires `type->validate()` to run before
  those "pm" checks — not before `/* listen */`, which already ran before them
  too.
- Fix: moved the `/* listen */` block in `fpm_conf.c`'s
  `fpm_conf_process_all_pools()` to run before `type->validate()` (and kept it
  before the "pm" checks, unchanged). `wp->listen_address_domain` is now
  populated by the time `fpm_http_validate_pool()` reads it, so `listen =
  host:port` with no `http.listen` is accepted and defaults to the FastCGI
  port + 1 (`fpm_http.c:1750`), while a unix-socket listen is still refused
  with the existing, now-accurate message.
- Added `sapi/fpmng/tests/fpmng-http-listen-default.phpt`: starts a TCP
  `pool.type = http` pool with no `http.listen` and confirms a request on the
  FastCGI port + 1 is served; separately confirms a unix-socket `pool.type =
  http` pool with no `http.listen` is still rejected with "pool.type = http
  requires http.listen when listen is a unix socket". Not run locally (no
  prepared php-src tree in this worktree) — verified through CI's
  `fpmng-phpt`/`phpt` jobs on the PR instead.
- Updated `docs/NOTES.md` (~line 2576, the original "found incidentally, not
  fixed" note) to point at this task as the fix.
- Left out: did not remove the now-redundant `http.listen` from existing test
  configurations that only carried it because of this bug — out of scope for
  this task, and removing it everywhere would be its own unrelated diff.
