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

## Two consumers

`build/prepare.sh` applies the stack to a php-src checkout. That is the
from-source build, and it is what branch async uses. `third_party/php-src/`
holds `main/fastcgi.c` and `main/fastcgi.h` with the stack **already applied**
at a pinned tag (issue #421), so a build against a distribution SDK needs no
php-src tree.

The manifest there records a fingerprint of this directory. A change here
therefore has to be followed by
`build/vendor-php-src.sh import <php-src checkout at the pinned tag>`. Until
then, `build/vendor-php-src.sh check` fails, and with it the CI checks job.
Never edit the vendored copies directly: the next import refuses to run
rather than overwrite the edit.

## Status

| patch | touches | waiting for | checked on |
|---|---|---|---|
| `0001-gh18956-fastcgi-keepalive-counting.patch` | `main/fastcgi.c`, `main/fastcgi.h`, **`sapi/fpm/fpm/fpm_request.c`, `fpm_request.h`** (full PR, not an excerpt) | https://github.com/bukka/php-src/pull/2 (GH-18956) | 8.4, 8.5, master; **8.3 through the** `php-8.3/` variant (7 arguments to `fpm_scoreboard_update_commit`) |
| `0002-fastcgi-tcp-nodelay-never-set.patch` | `main/fastcgi.c` | report to php/php-src — text ready in `0002-upstream-report.md`, not sent yet | 8.3, 8.4, 8.5, master |

The stack is ordered: 0002 assumes 0001 has already been applied (the
context around `accept()`), although they are independent in substance.
`prepare.sh` applies everything in order to an untouched tree, and checks
"already applied" for the whole stack at once (in reverse, from copies of the
touched files) — a per-patch test lies when two patches occupy the same location.

Patches 0003 (buffered read, `accept4`), 0004 (`fcgi_set_optimized_transport()`)
and 0005 (`writev` for large responses) were dropped from `main` by issue #589:
nothing has selected the optimized transport since #420 (#376 retired
`fastcgi-ng`, #388 retired `pool.type = http`), and `main` carries no patch it
does not need for correctness. Branch `async` may keep its own copies. The
numbers they earned are in `docs/NOTES.md` (3t and the `writev` section).

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

PHP-8.3 currently has ONE variant (`0001` — 7 arguments to
`fpm_scoreboard_update_commit`); a second one (`0003`, `const void *buf` in
`safe_read()`) went with that patch in issue #589. It will disappear with the
patch when upstream merges it; if 8.3 diverges further, dropping 8.3 from
supported versions will be cheaper than a third variant.

**DECISION (2026-09-05): KEEP 8.3.** It still receives security support, and the
two variants cost us practically nothing — they apply cleanly and are covered
by CI. The rule-3 threshold remains a warning, not an automatic cutoff: merely
being at the threshold is NOT a reason to drop a version. The reason would be a
third variant or a variant requiring different logic rather than a different
signature. Until then, do not reopen this discussion.
