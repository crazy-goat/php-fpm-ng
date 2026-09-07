# patches/

Patches for php-src files **outside `sapi/`**. Normally fpm-ng does not touch
upstream — `prepare.sh` creates only `sapi/fpmng/`. These patches are an
exception and must remain visible, measurable, and temporary.

## Rules

1. **Every patch has an expiry path.** Its header names the upstream PR it is
   waiting for. It disappears when that PR is merged. A patch with no exit path
   is a hidden fork.
2. **One patch per problem**, not one patch per PHP version. Versions are handled
   only when the same patch stops applying.
3. **More than two version variants of one patch is a warning sign** — either send
   it upstream or redesign it so it fits in `sapi/fpmng/`.
4. **CI builds every supported PHP version**, so a patch that no longer applies
   fails during the build, not at release time.

## Layout

```
patches/*.patch              always applied
patches/php-8.4/*.patch      only for that version; overrides the same-named patch
```

## Status

| patch | touches | waiting for | checked on |
|---|---|---|---|
| `0001-gh18956-fastcgi-keepalive-counting.patch` | `main/fastcgi.c`, `main/fastcgi.h`, **`sapi/fpm/fpm/fpm_request.c`, `fpm_request.h`** (full PR, not an excerpt) | https://github.com/bukka/php-src/pull/2 (GH-18956) | 8.4, 8.5, master; **8.3 through the** `php-8.3/` variant (7 arguments to `fpm_scoreboard_update_commit`) |
| `0002-fastcgi-tcp-nodelay-never-set.patch` | `main/fastcgi.c` | report to php/php-src — text ready in `0002-upstream-report.md`, not sent yet | 8.3, 8.4, 8.5, master |
| `0003-fastcgi-buffered-read-accept4.patch` | `main/fastcgi.c` | candidate PR to php/php-src, not submitted | 8.4, 8.5, master; **8.3 through the** `php-8.3/` variant (different `safe_read` signature) |
| `0004-fastcgi-ng-transport-switch.patch` | `main/fastcgi.c`, `main/fastcgi.h` | move the switch behind an API owned by `sapi/fpmng` | 8.5, master; older versions to verify |
| `0005-fastcgi-writev-large-response.patch` | `main/fastcgi.c` | candidate PR to php/php-src, not submitted | 8.5, master; older versions to verify |
| `0006-zend-persistent-signal-handlers.patch` | `Zend/zend_signal.c`, `zend_signal.h` | move the switch behind an API owned by `sapi/fpmng`, or propose it upstream | 8.5; older versions and master to verify |

The stack is ordered: 0002 and 0003 assume 0001 has already been applied (the
context around `accept()`), although they are independent in substance.
`prepare.sh` applies everything in order to an untouched tree, and checks
"already applied" for the whole stack at once (in reverse, from copies of the
touched files) — a per-patch test lies when two patches occupy the same location.

### Why 0001 is necessary

The gateway keeps persistent connections to the pool (`FCGI_KEEP_CONN`), so
fpm-ng is exactly the case broken by GH-18956: the idle-versus-active counter
lies, and `pm = dynamic` and `ondemand` scale the pool incorrectly. Without this
patch, only `pm = static` is trustworthy.

Information about whether `accept` came from a persistent connection lives in
`main/fastcgi.c` — it cannot be produced from `sapi/` alone. The companion
changes in `fpm_request.c` and `fpm_request.h` are carried as our own files in
`sapi/fpmng/fpm/`, because they live under `sapi/`.

### Why 0002 (`TCP_NODELAY`)

An upstream bug, not our optimization: `req->tcp` is assigned only under
`_WIN32`, so on Linux `TCP_NODELAY` is never applied to a keep-alive connection.
A response larger than 8 KB over TCP uses several `write()` calls; the final
small segment waits for ACK: Nagle + delayed ACK, tens of milliseconds instead
of microseconds. The gateway keeps TCP connections to the pool, so this affects
us directly. Report and reproducer: `0002-upstream-report.md`.

### Why 0003 (input buffer + `accept4`)

A purely transport-level syscall saving in the worker (one `read()` for the
request header instead of six, `accept4(SOCK_CLOEXEC)` instead of `accept` +
2x `fcntl`), with no wire changes. Before/after numbers: `docs/NOTES.md`,
section 3t. Our `sapi/fpmng/config.m4` detects `accept4`
(`AC_CHECK_FUNCS([accept4])`) because upstream checks it only in `ext/sockets`;
without `HAVE_ACCEPT4`, the old path is compiled. An upstream version would
need to add this check to `configure.ac`.

### Why 0006 (persistent signal handlers)

Zend normally checks and registers seven handlers again when every request is
activated. `fastcgi-ng` and `http` enable a process-wide switch after which full
registration happens only for the worker's first request; `SIGPROF` for the
timeout is still set later. Classic `fastcgi` does not enable this path. The
change reduces `fastcgi-ng` from about 25.2 to 18.2 syscalls/request and
repeatedly produced about 7% less CPU/request.

Logical Zend handlers are still reset per request, confirmed with
`pcntl_signal()`. The trade-off is that an extension replacing a handler through
direct libc `sigaction()` is not automatically repaired with the default
`zend.signal_check=0`; explicit `zend.signal_check=1` still detects the change
during request shutdown.

### RESOLVED (path 1): 0001 broke `--enable-fpm --enable-fpmng` in one tree

Coordinator decision: path 1. `0001` now also carries the PR hunks for
`sapi/fpm/fpm/fpm_request.c/.h`, so it is the FULL equivalent of bukka#2's PR
(without `.phpt` tests) and disappears entirely when that PR is merged. Both
SAPIs build together and the old FPM receives the same fix. Verification:
`--enable-fpm --enable-fpmng` build + the full `sapi/fpm/tests` suite — result in
`docs/NOTES.md` 3t. The original analysis below is retained for context.

`0001` changes the `fcgi_init_request()` hook signatures in `main/fastcgi.h` from
`void(*)(void)` to `void(*)(bool)`. Upstream `sapi/fpm/fpm/fpm_main.c` passes
`fpm_request_accepting`/`fpm_request_reading_headers` with the old signatures
from upstream `fpm_request.h`, so GCC 14+ stops the build
(`-Wincompatible-pointer-types` is an error). The promise that "old FPM keeps
working alongside it" is broken here: today only `--enable-fpmng` works without
`--enable-fpm`. Our `sapi/fpmng` compiles because it carries its own
`fpm_request.c/.h` with `bool` signatures.

Size: **small**, because it does exactly what the upstream PR does. PR bukka#2
(GH-18956) changes `main/fastcgi.c` **and** `sapi/fpm/fpm/fpm_request.c`,
`sapi/fpm/fpm/fpm_request.h` (plus tests). Our `0001` was the `main/` portion;
we carried the missing part as our own files in `sapi/fpmng/` — which is why
upstream `sapi/fpm` fell behind.

Two paths:

1. **Add the PR hunks for `sapi/fpm/fpm/fpm_request.c` and `fpm_request.h` to
   `0001`** (~33 lines, identical to what we have in `sapi/fpmng/`). Both SAPIs
   then build together, and upstream FPM in the same tree gets the GH-18956
   counting fix — behavior upstream will merge anyway. Cost: the patch touches
   `sapi/fpm/` for the first time, but it remains one problem = one patch, with
   the same expiry path. The old FPM behavior changes only through correct
   idle/active counters on keep-alive.
2. **Redesign `0001` without changing signatures**: keep the old hooks and add a
   separate setter in `fastcgi.c` (for example,
   `fcgi_request_set_hooks_ex(req, on_accept(bool), on_read(bool))`). Upstream
   `sapi/fpm` stays untouched, but our copied `fpm_main.c` calls
   `fcgi_init_request()` with old prototypes and our `bool` functions — so we
   would have to own `fpm_main.c` (20 commits/year) or add a hook to it. More
   code, further from upstream's shape.

Recommendation: path 1. Zero new code, convergent with upstream, and
`prepare.sh` could stop deleting `sapi/fpmng/tests` merely because the tests
refer to the php-fpm binary — with `--enable-fpm` alongside it, upstream tests
run in the same tree. Do this after the coordinator's decision, not in this task.

### Version variants

PHP-8.3 currently has TWO variants (`0001` — 7 arguments to
`fpm_scoreboard_update_commit`; `0003` — `const void *buf` in `safe_read()`).
That is exactly the warning threshold from rule 3. Both will disappear with the
patches when upstream merges them; if 8.3 diverges further, dropping 8.3 from
supported versions will be cheaper than a third variant.

**DECISION (2026-09-05): KEEP 8.3.** It still receives security support, and the
two variants cost us practically nothing — they apply cleanly and are covered
by CI. The rule-3 threshold remains a warning, not an automatic cutoff: merely
being at the threshold is NOT a reason to drop a version. The reason would be a
third variant or a variant requiring different logic rather than a different
signature. Until then, do not reopen this discussion.
