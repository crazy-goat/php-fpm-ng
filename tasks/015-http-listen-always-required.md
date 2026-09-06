# 015 — `http.listen` is effectively always required, contradicting its own error message

**Priority:** low. Small, self-contained, and user-facing.
**Status:** open. Found while doing something else and deliberately not fixed
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
