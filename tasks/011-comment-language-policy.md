# 011 — Settle the comment language and stop the drift

**Priority:** medium. Cheap, and it is already costing time.
**Status:** open.

## Context

Comment language in `sapi/fpmng/fpm/` is inconsistent, and not along any
principled line. A rough count of Polish-marker versus English-marker lines per
file:

    fpm_http.c              PL 16   EN 138
    fpm_conf.c              PL  2   EN  54   (mostly inherited from upstream)
    fpm_process_ctl.c       PL  0   EN  25   (inherited from upstream)
    fpm_pool_coop.c         PL 54   EN   7
    fpm_pool_cron.c         PL 33   EN   1
    fpm_pool_coop_ini.c     PL 29   EN   0
    fpm_pool_coop_session.c PL 28   EN   0
    fpm_pool_fiber_xport.c  PL 26   EN   6

So: the `fpm_http*` family was written in English, the `fpm_pool_*` family in
Polish, and files copied from upstream carry upstream's English.

This already causes friction — contributors have to be told "match the language
of the file you are editing" before touching anything, which is a rule that
exists only because no decision was made.

## Problem

Pick one language for comments in this project's own code, write the rule down,
and make it enforceable so the tree does not drift back.

## Acceptance criteria

1. A decision, recorded in a place a contributor will actually read
   (`sapi/fpmng/README.md` and/or `CLAUDE.md`), covering:
   - the language for **our** files
   - what happens to comments **inherited from upstream** in copied files —
     leaving them untouched is a legitimate answer, and probably the right one,
     since `build/prepare.sh` re-copies those files from `sapi/fpm/` on every run
   - whether commit messages are in scope (they are currently Polish)
2. A check that fails when a new comment violates the rule. It does not need to
   be clever — detecting Polish diacritics is not enough, since this codebase
   deliberately writes Polish without them. Whatever the check is, it must be
   cheap enough to run on every commit and must not fire on upstream's files.
3. No mass rewrite in this task. Translation of existing comments is task 012
   and is deliberately separate, so that the rule lands before the churn.

## Notes

- The realistic answer is English, because it is the only choice that keeps the
  option of ever making this repository public or accepting an outside
  contributor. But it should be an explicit decision with that reason attached,
  not a default.
- Whatever is decided, files copied from `sapi/fpm/` by `prepare.sh` are
  regenerated every run. Any rule that would require editing them is a rule that
  cannot hold.
