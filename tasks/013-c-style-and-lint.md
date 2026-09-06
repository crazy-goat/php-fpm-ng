# 013 — C style rules and a linter, matching upstream rather than inventing

**Priority:** medium-low.
**Status:** open.

## Context

There is no style configuration in this repository. php-src itself ships an
`.editorconfig` and **no** `.clang-format`.

The whole design of `build/prepare.sh` rests on our files looking like FPM's
files: it copies `sapi/fpm/` wholesale and lays our overlay on top, so our code
sits next to upstream code in one directory and gets diffed against it. A style
that diverges from upstream's makes that comparison harder for no gain.

Compilation already runs with `-Wall -Wextra` plus php-src's set
(`-Wstrict-prototypes -Wformat-truncation -Wlogical-op -Wduplicated-cond`,
visible in the link line of any build).

One known benign warning exists and should not be "fixed":
`sapi/fpmng/fpm/fpm_pool_coop.c:426`, `errno == EAGAIN || errno == EWOULDBLOCK`
under `-Wlogical-op` — the two constants are equal on Linux and the idiom is
deliberate portability.

## Problem

Establish enforceable style and static-analysis rules that match php-src.

## Acceptance criteria

1. `.editorconfig` present, consistent with php-src's (tabs, width, line
   endings, final newline).
2. A decision on `clang-format`, with reasoning either way. If adopted: it must
   apply **only** to our own files, never to files `prepare.sh` copies from
   upstream, and the mechanism enforcing that boundary is part of the task.
3. A static-analysis pass (`clang-tidy` or similar) configured with a **chosen
   subset** of checks, not the default set. Every enabled check has a reason;
   the list of deliberately disabled ones with their reasons is as valuable.
4. Known-benign findings are suppressed at the site with a comment explaining
   why, not globally.
5. Runs in CI (task 002) as a non-blocking report first. Turning any check into
   a hard failure is a separate, later decision — a red build on day one for
   pre-existing findings teaches people to ignore it.
6. A note in `sapi/fpmng/README.md` telling a contributor how to run the checks
   locally.

## Explicitly out of scope

- Reformatting the existing tree. Any bulk reformat destroys `git blame` on a
  codebase whose comments are its main documentation. If the tree is
  reformatted at all, it is its own commit, done once, and recorded in
  `.git-blame-ignore-revs`.
- Style rules for the PHP test files — those follow php-src's test conventions.

## Notes

- Comment-content rules (what deserves a comment) are **not** a linter's job and
  belong in `CLAUDE.md`. See 014.
