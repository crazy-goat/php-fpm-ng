# 018 — Two gaps left by the HTTP gateway's front-controller fallback

**Priority:** low.
**Status:** open. Both noticed while reviewing the fallback that landed on
2026-09-06 (`http.front_controller`).

## Context

`http.front_controller` (default `/index.php`, empty disables it) makes a request
whose computed `SCRIPT_FILENAME` does not exist fall back to the front
controller, with the original path in `PATH_INFO` — the equivalent of nginx's
`try_files $uri /index.php$is_args$args`. Verified: `/mix?x=1` yields
`SCRIPT_NAME=/index.php`, `PATH_INFO=/mix`, `REQUEST_URI=/mix?x=1`.

Two things were left behind.

## Gap 1 — an existing directory without an index does not fall back

For a URL like `/somedir` where `somedir` is a real directory containing no
`index.php`, `stat()` succeeds, so the fallback does not trigger and the request
goes to the worker as a directory path, producing "File not found".

Under nginx's `try_files $uri /index.php` the same URL reaches the front
controller. PHP's own built-in server also handles it, by a different route: it
walks the path leftwards and, on hitting a directory, tries `index.php` then
`index.html` (`sapi/cli/php_cli_server.c`, `php_cli_server_request_translate_vpath()`).

This is not a regression — it behaved the same before the fallback existed — but
it is a difference from both reference implementations, and it is undocumented.

## Gap 2 — the front controller's containment check is lazy

`fpm_http_front_controller_ok()` validates that the configured front controller
resolves inside the document root, and caches the verdict in a function-level
static, evaluated on the **first request**. A configuration error is therefore
reported at first request rather than at startup.

Configuration validation already rejects the textual cases (a non-absolute path,
`..`) at validation time, before any fork. Only the `realpath()`-based
containment check — the one that catches a symlink pointing outside the document
root — is deferred, because resolving the document root needs the child's
context.

## Problem

Decide what the gateway should do for a directory without an index, and whether
the containment check can move earlier.

## Acceptance criteria

1. A decision on gap 1, with reasoning, between: falling back to the front
   controller (matching nginx), trying directory index files (matching `php -S`),
   or keeping today's behaviour and documenting it. Note that this project has
   already chosen "behave like `php -S` unless there is a reason not to" once,
   when making the fallback default-on.
2. Whatever is chosen, it must not add a syscall to the common GET path. The
   fallback currently costs **zero** extra syscalls for GET/HEAD with
   `http.static = 1`, because it reuses the `realpath()` the static-file path
   already performed; one extra `stat()` elsewhere. That property is the reason
   the design was accepted and must survive.
3. A decision on gap 2: either the containment check moves to startup (once the
   document root is resolvable), or the lazy evaluation stays and the reason is
   written in the code where the static cache lives.
4. If the check stays lazy, a misconfiguration must still be *loud* — today it
   logs a warning and disables the fallback, which is the right shape; confirm
   that the warning is visible in a normal deployment and not swallowed.

## Notes

- The function-level static caching the verdict is safe today only because each
  gateway process serves exactly one pool. If that ever stops being true, the
  cache becomes wrong. Worth a comment at minimum, or a field on the gateway
  struct instead.

## Outcome

Both gaps closed in `sapi/fpmng/fpm/fpm_http.c`.

**Gap 1** — decided to match nginx's `try_files $uri /index.php`, not `php
-S`'s directory-index walk: a directory (with or without its own nested
`index.php`) never matches `$uri`, so it now falls back to
`http.front_controller` the same way a missing file already did. Not probing
a directory for its own `index.php` is a deliberate simplification, made
specifically to keep the "must not add a syscall to the common GET path"
constraint intact — a per-directory index check would cost an extra
`stat()`/`access()` per request.

Implementation reused syscalls already being paid for rather than adding new
ones:
- `fpm_http_serve_static()` already `fstat()`s the resolved path to reject
  non-regular files (`fpm_http.c:1280`). It now inspects that same result:
  `S_ISDIR` sets `*script_missing = 1` instead of just falling through, so
  `fpm_http_build_request()`'s GET/HEAD +
  `http.static = 1` fast path (the common case) gets the directory verdict
  for free, exactly like it already got the missing-file verdict for free.
- `fpm_http_build_request()`'s own fallback `stat()` (the path already
  documented as "the one extra stat() the fallback adds") now also treats
  `S_ISDIR` as missing. This is the pre-existing non-common-path cost, not a
  new one.

**Gap 2** — moved to startup. `gw->docroot` and `gw->front_controller` are
both already resolved in the master, in `fpm_http_init_pool_ex()`, before any
gateway process forks (confirmed: that function's own comment says so, and
nothing between there and `fpm_http_gateway_run()` does a `chdir()`/`chroot()`
that would make the master's filesystem view differ from a gateway child's —
the HTTP gateway is never chrooted, only classic FastCGI workers are). So the
containment check no longer needs "the child's context" the task assumed it
did; `fpm_http_front_controller_ok()` (a lazy function-static) became
`fpm_http_front_controller_validate()`, called once from
`fpm_http_init_pool_ex()` right after `fpm_http_gateway_settings()` sets
`gw->front_controller`, storing the verdict in a new `gw->front_controller_ok`
field that `fork()` then hands to every gateway process already decided —
mirroring how `fpm_http_tls_validate()` already validates TLS cert/key
problems at pool-validation time instead of at first request. Severity is
unchanged: still a `zlog(ZLOG_WARNING, ...)` that disables the fallback, not a
hard startup failure — only the timing moved. This also retires the "safe
only because each gateway process serves exactly one pool" caveat from the
Notes above: the value is now computed once, period, not once-per-process.

Verified on the test box (192.168.8.50, own scratch directory, upstream
php-src `php-8.5.9` fetched fresh rather than reusing the box's pre-existing
`~/php-src` checkout, which turned out to already carry unrelated local
changes and would have produced a false "measurement" — see CLAUDE.md's
warning about this): a new test,
`sapi/fpmng/tests/http-front-controller-directory-and-startup.phpt`, drives a
real HTTP gateway process end-to-end over plain TCP.
- Gap 1: `GET /somedir` (empty dir), `POST /somedir` (exercises the
  non-`fpm_http_serve_static` codepath), and `GET /withindex` (a directory
  that does contain its own `index.php`) all fall back to `/index.php` with
  `PATH_INFO` carrying the original path. `GET /`, `GET /real.php`, and
  `GET /withindex/` (trailing slash, unaffected by this task) confirmed
  unchanged.
- Gap 2: a pool whose `http.front_controller` is a symlink resolving outside
  its document root logs the containment warning and reaches "ready to handle
  connections" without ever receiving a single request — proving the warning
  fires at startup, not lazily.

Full upstream FPM `.phpt` suite against this build: PASS=126 FAIL/ERROR=0
SKIP=20 WARN=1 (`log-bwd-multiple-msgs-stdout-stderr.phpt`, pre-existing and
unrelated — also warned on the pre-change binary).

Not measured: raw syscall counts before/after (e.g. via `strace -c`) for the
directory-fallback path, to confirm zero-additional-syscall empirically rather
than by code inspection alone.
