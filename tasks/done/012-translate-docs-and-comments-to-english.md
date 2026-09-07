# 012 — Translate documentation and our own comments to English

**Priority:** medium. Unblocked — the rule now exists.
**Status:** done. The language decision was taken on 2026-09-06 (English,
everywhere) and is recorded in `CLAUDE.md`. This task is the translation work
itself; task 011 is the check that stops the tree drifting back.

## Context

Effectively all of `docs/` is Polish: `NOTES.md`, `fiber_errors.md`,
`frameworks.md`, `async_errors.md`, `fiber_async_io.md`, `spike-tsrm-context.md`.
So are most comments in `sapi/fpmng/fpm/fpm_pool_*.c` (see 011 for the split).

That was acceptable for a private single-author project and is a hard stop for
anything else — a public repository, an outside contributor, an upstream
discussion. The decision has been made to move everything to English.

Sizes, largest first, so the work can be planned:

    docs/NOTES.md                     3429 lines
    docs/fiber_errors.md               422
    docs/FASTCGI_NG_OPTIMIZATION.md    299
    docs/spike-tsrm-context.md         272
    docs/frameworks.md                 262
    docs/node_server_gaps.md           207
    docs/fiber_async_io.md              87
    docs/async_errors.md                87
    README.md                           82   (already partly English)
    sapi/fpmng/README.md                23

plus the Polish comments in `sapi/fpmng/fpm/`, concentrated in the `fpm_pool_*`
files (see the table in task 011).

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

## Outcome

All owned text is English, translated file by file in individual commits
reviewable against the originals:

- `README.md`, `sapi/fpmng/README.md`, all of `docs/` (`NOTES.md`,
  `frameworks.md`, `fiber_errors.md`, `async_errors.md`, `fiber_async_io.md`,
  `spike-tsrm-context.md`, `node_server_gaps.md`,
  `FASTCGI_NG_OPTIMIZATION.md`, `cron.md`, `patches/README.md`)
- all owned comments in `sapi/fpmng/fpm/**` (pool families, HTTP gateway,
  cron schedule, watchdog, coop layers, metrics glue), including headers
- `ext/fpmng_metrics/**` comments
- `build/prepare.sh` (comments and its own error/log messages — they are
  ours to change), `build/*.sh`, `sapi/fpmng/config.m4`,
  `ext/fpmng_metrics/config.m4`, workflow comments
- `tasks/` including quotations of historical Polish material, translated
  rather than allow-listed (decision 2026-09-07: English everywhere means
  quotations too)
- commit messages on this branch are English and are checked by the task 011
  checker in CI

Numbers, error strings, paths and `file:line` references survived verbatim;
quoted program output inside docs is untouched (checker skips code blocks
for the lexicon and only flags diacritics there). The measured/reasoned/
assumed distinctions were preserved in translation (`MEASURED` stays
measured, hedges stay hedges).

Enforcement: `python3 build/check-english.py` over the full tree reports
0 findings; `git diff --check` clean; all build scripts pass `sh -n`.
