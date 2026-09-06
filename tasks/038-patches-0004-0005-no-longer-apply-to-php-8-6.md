# 038 — Patches 0004 and 0005 no longer apply to PHP 8.6.0-dev, blocking `prepare.sh`

**Priority:** high. `build/prepare.sh` is the only way to assemble this project
into a php-src tree, and it stops with `exit 1` on the current checkout. Nobody
can build against php-src master until this is resolved.
**Status:** open.

## Context

`patches/` holds the small number of changes this project makes outside
`sapi/fpmng/`. `build/prepare.sh` applies the whole stack as a unit and refuses
to continue if any patch does not apply, deliberately: the alternative is a
binary that is quietly missing a change.

Two of them no longer apply. Measured on `main/php_version.h` reporting
`PHP_VERSION "8.6.0-dev"`:

```
patches/0004-fastcgi-ng-transport-switch.patch
    3 out of 6 hunks failed while patching 'main/fastcgi.c'

patches/0005-fastcgi-writev-large-response.patch
    1 out of 4 hunks failed while patching 'main/fastcgi.c'
```

Both target `main/fastcgi.c`. All six patches apply cleanly to PHP 8.5.9, which
is how the build-flag work was verified — on a separate checkout pinned to that
tag, not on the working tree.

`prepare.sh` handles this correctly: it detects the failure and exits rather
than producing a half-patched tree, and its message already points at the two
legitimate outcomes. This task is about choosing between them, not about the
detection.

## What has to be decided

`patches/README.md` documents the rules and the expiry conditions for each
patch. Whoever picks this up reads that first, then decides per patch:

- **Upstream merged it.** Then the patch is deleted, not ported. Check
  `main/fastcgi.c` history for the change rather than assuming.
- **Upstream moved the code.** Then a `patches/php-8.6/` variant is added.
  `prepare.sh` already prefers a version-specific variant over the generic one,
  so the mechanism exists and needs no work.
- **The patch is no longer wanted.** Then it goes, and whatever depended on it
  is re-measured.

The failing hunks are the interesting evidence: three of six in one patch and
one of four in the other means the surrounding code moved rather than the whole
function being rewritten, so this is more likely a rebase than a redesign. That
is a guess from hunk counts, not from reading the diff — whoever picks it up
should confirm it.

## Acceptance criteria

1. `build/prepare.sh <php-src at 8.6.0-dev>` completes without error and reports
   either that patches were applied or that the tree is untouched.
2. A default build (`--enable-fpmng`, neither executor flag) from a clean build
   directory produces a working binary: a `fastcgi` pool and an `http` pool each
   serve a request.
3. For every patch that stays, `patches/README.md` states which PHP versions it
   is known to apply to, and that claim is checked, not assumed.
4. For every patch that is deleted, the commit message says why — merged
   upstream (with the upstream commit named) or no longer wanted (with what was
   re-measured).
5. The 8.5 line still works, or the task records deliberately that it no longer
   does and why that is acceptable.

## Notes

- Found by an independent verification of the fiber/async build flags, which had
  to clone php-src and pin it to 8.5.9 to get a buildable tree. That workaround
  should not become the normal way to build this project.
- This is unrelated to the build flags themselves; the patch mechanism is a
  separate thing that happened to be exercised at the same time.
