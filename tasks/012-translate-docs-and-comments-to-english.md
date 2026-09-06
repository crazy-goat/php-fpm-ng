# 012 — Translate documentation and our own comments to English

**Priority:** low-medium. Do it after 011, never before.
**Status:** open. Blocked on 011 (the rule must exist first).

## Context

Effectively all of `docs/` is Polish: `NOTES.md`, `fiber_errors.md`,
`frameworks.md`, `async_errors.md`, `fiber_async_io.md`, `spike-tsrm-context.md`.
So are most comments in `sapi/fpmng/fpm/fpm_pool_*.c` (see 011 for the split).

That is fine for a private single-author project and a hard stop for anything
else — a public repository, an outside contributor, an upstream discussion.

## Problem

Translate, without losing what the text is actually worth.

## Why this is not a mechanical job

These documents are not prose about intentions. They are a record of
**measurements and rejected directions**, and their value is in the precision:

- "20/20 rounds HTTP 500, `Cannot call session save handler in a recursive manner`"
- "`auto_globals_jit = off` does **not** help — measured"
- why `ts_resource_ex` cannot be used and what exactly segfaults
- why the `require_once` value cache was rejected for Symfony specifically

A bulk translation pass over ~14 000 lines of code plus several thousand lines of
docs will blur exactly these statements, because they are the longest and most
technical sentences in the corpus. The result would read fine and mean less.

## Acceptance criteria

1. Translation happens **incrementally**, file by file, each in its own commit,
   so that any single translation can be reviewed against the original.
2. Every number, error message, file path, `file:line` reference and measurement
   survives verbatim. Error strings and log lines are quoted, never translated.
3. The distinction between *measured*, *reasoned* and *assumed* survives. Where
   the Polish says "zmierzone" the English must say measured; where it hedges,
   the English must hedge. Upgrading a hedge to a claim is the specific failure
   to avoid.
4. Priority order, most valuable first:
   - `README.md` (already partly English)
   - `docs/frameworks.md` and `docs/fiber_errors.md` — the two documents an
     outsider would need first
   - `sapi/fpmng/README.md`
   - our own comments in `sapi/fpmng/fpm/`
   - `docs/NOTES.md` last: it is the working journal, the largest, and the least
     useful to anyone outside the project
5. Comments inherited from upstream in copied files are not touched — see 011.

## Explicitly out of scope

- Rewriting or shortening while translating. If a document is wrong or stale,
  that is a separate change with its own commit, so it is visible.
