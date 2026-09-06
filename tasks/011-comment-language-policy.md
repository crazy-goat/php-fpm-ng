# 011 — Enforce the English-only rule so the tree does not drift back

**Priority:** medium. The decision is made; only enforcement is left.
**Status:** open. **Decision taken 2026-09-06: English, everywhere.** Recorded in
`CLAUDE.md`.

## The decision

English is the only language in this project: code comments in our own files,
`docs/`, `README.md`, `sapi/fpmng/README.md`, `tasks/`, commit messages, log and
error messages, configuration documentation.

Two exceptions, both about files we do not own:

- comments inherited from upstream in files `build/prepare.sh` copies from
  `sapi/fpm/` — that script re-copies them every run, so edits there are lost
- quoted material: error strings, log lines, command output, measurements. Never
  translate something that appears in a program's output.

## Why enforcement is a separate problem

The tree drifted into two languages without anyone deciding it, and the split
does not follow any principle. A count of Polish- versus English-marker lines
per file at the time of the decision:

    fpm_http.c              PL 16   EN 138
    fpm_conf.c              PL  2   EN  54   (inherited from upstream)
    fpm_process_ctl.c       PL  0   EN  25   (inherited from upstream)
    fpm_pool_coop.c         PL 54   EN   7
    fpm_pool_cron.c         PL 33   EN   1
    fpm_pool_coop_ini.c     PL 29   EN   0
    fpm_pool_coop_session.c PL 28   EN   0
    fpm_pool_fiber_xport.c  PL 26   EN   6

The `fpm_http*` family was written in English, the `fpm_pool_*` family in Polish.
Without a check, new files will keep landing in whichever language the author
was thinking in — as they have been.

## Problem

Add a check that fails when new Polish text lands in a file we own.

## Acceptance criteria

1. A check exists and runs on every commit, or in CI (task 002), or both.
2. It does **not** fire on files copied from upstream by `prepare.sh`. The list
   of files we own is derivable — `sapi/fpmng/` in this repository is exactly
   our overlay — so the exclusion should be mechanical, not a hand-maintained
   list of exceptions.
3. Detecting Polish diacritics is **not sufficient**: this codebase deliberately
   writes Polish without them (`zeby`, `ktore`, `wiec`). Whatever heuristic is
   used must catch that, and its false-positive behaviour on technical English
   must be checked against the real tree before it is turned on.
4. The check tolerates the current state or is introduced together with the
   translation of the files it covers. A check that fails on every existing file
   from day one is a check people learn to bypass.
5. Commit messages are covered, or explicitly excluded with a reason.

## Explicitly out of scope

- Translating existing text. That is task 012, deliberately separate so the rule
  lands before the churn.

## Notes

- Consider whether the check should be a git hook, a CI step, or both. A hook
  gives fast feedback but is not enforced for anyone who does not install it;
  CI is authoritative but slow. Both is defensible; pick and say why.
