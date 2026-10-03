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
| (none) | | | |

`prepare.sh` applies everything in order to an untouched tree, and checks
"already applied" for the whole stack at once (in reverse, from copies of the
touched files) — a per-patch test lies when two patches occupy the same location.

Patch 0002 (`TCP_NODELAY` on FastCGI keep-alive connections) was dropped from
`main` by issue #590: the same effect now comes from `.listening_socket_nodelay = 1`
on the `fastcgi` pool type in `sapi/fpmng/fpm/fpm_pool_type.c`, because accepted
sockets inherit the option from the listener on Linux
(`sapi/fpmng/tests/fpmng-fastcgi-tcp-nodelay.phpt`; macOS/BSD: not verified).
The upstream bug is still real: `patches/0002-upstream-report.md` is the report
to send to php/php-src, and the patch and reproducer it names are in git history
(`git show bd3f332:patches/0002-fastcgi-tcp-nodelay-never-set.patch`,
`git show bd3f332:patches/0002-fcgi-nodelay-repro.py`).

Patches 0003 (buffered read, `accept4`), 0004 (`fcgi_set_optimized_transport()`)
and 0005 (`writev` for large responses) were dropped from `main` by issue #589:
nothing has selected the optimized transport since #420 (#376 retired
`fastcgi-ng`, #388 retired `pool.type = http`), and `main` carries no patch it
does not need for correctness. Branch `async` may keep its own copies. The
numbers they earned are in `docs/NOTES.md` (3t and the `writev` section).

### Patch 0001 (GH-18956) was dropped from `main` by issue #591

`main` carries no php-src patch. `patches/0001-gh18956-fastcgi-keepalive-counting.patch`
(and its `php-8.3/` variant) fixed the idle/active counters on FastCGI keep-alive
connections. The upstream bug is still real and is reported as php/php-src#18956
(fix: bukka/php-src#2); the patch is in git history
(`git log --diff-filter=D -- patches/0001-gh18956-fastcgi-keepalive-counting.patch`).

What the unpatched counters cost (static reading, not measured):

- pm scaling is not affected: the maintenance loop recounts idle and active
  processes from `request_stage` on every heartbeat
  (`sapi/fpmng/fpm/fpm_process_ctl.c`).
- Pristine `fastcgi.c` calls `on_read()` before the blocking read on a kept
  connection (`third_party/php-src/main/fastcgi.c`), so a worker that starts
  waiting on a kept connection counts as reading headers. Hence `accepted conn`
  and the per-process `requests` get one extra request for every kept connection
  the client closes (the gateway does this after `http.idle_timeout`, 500 ms by
  default), a waiting worker is shown as `Reading headers` with the idle wait in
  `request duration`, and `max active processes` stays too high.
- `idle processes` and `active processes` in the status page can be wrong for
  up to one heartbeat (about 1 s).
- `request_terminate_timeout` can hit an idle kept-alive worker only with
  `http.idle_timeout = 0`, a large value, or an external proxy using
  `fastcgi_keep_conn on`.

`sapi/fpmng/fpm/fpm_request.c` offers the `void` entry points
`fpm_request_accepting()` and `fpm_request_reading_headers()` that the pristine
`fpm_main.c` passes to `fcgi_init_request()`, plus the `_ex(bool)` variants the
http-direct executors use.

### Version variants

None. The `php-8.3/` directory is gone with 0001. The 2026-09-05 decision to keep
PHP 8.3 supported stands; the rule-3 warning threshold applies again when a
patch needs variants.
