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
