# 014 — Write down what a comment is for, instead of pruning comments

**Priority:** low. Small, but it prevents an expensive mistake.
**Status:** done.

## Context

"Clean up redundant comments" has come up as a maintenance idea. On this
codebase that is a trap, and it is worth writing down why before somebody runs
the pass.

Comments here are the **primary record of why**, and most of what they record
cannot be recovered from the code. Examples from a single day's work:

- why `ts_resource_ex()` cannot be used and what exactly segfaults
  (`docs/spike-tsrm-context.md`, referenced from the code)
- why the address of `ps_globals` is taken from an ini entry's `mh_arg2` rather
  than by symbol, and what would break with `--enable-session=shared`
  (`fpm_pool_coop_session.c`)
- that `session.auto_start` bypasses a `session_start()` block entirely, because
  RINIT runs once per process in this executor
- that `-Wlogical-op` at `fpm_pool_coop.c:426` is benign because `EAGAIN` and
  `EWOULDBLOCK` are equal on Linux
- that pool `php_admin_value` settings do **not** land in
  `EG(modified_ini_directives)`, because `fpm_php_zend_ini_alter_master()`
  writes `ini_entry->value` directly — the fact that makes per-request ini
  restoration safe

Each of these took real effort to establish. A pass that ranks comments by
apparent redundancy would delete precisely these, because they are the longest.

The genuinely redundant kind — a comment restating the line below it — is rare
here, because the code was not written that way.

## Problem

Record the rule, so that the next person who feels the urge to prune has
something to prune *against*.

## Acceptance criteria

1. A short section in `CLAUDE.md` stating what earns a comment in this codebase.
   The working formulation:
   - **Keep:** it justifies a decision, cites a measurement, names a rejected
     alternative and why, or warns about a trap that the code cannot express.
   - **Delete:** it restates what the next line does.
   - **Never:** delete a comment containing a number, an error message, or a
     `file:line` reference without independently re-establishing the fact.
2. The rule says explicitly that there is **no** scheduled comment-cleanup pass.
   Redundant comments are removed opportunistically, while touching that code,
   in the same commit as the real change.
3. Linked from `tasks/README.md` and `sapi/fpmng/README.md`, so it is visible to
   anyone about to work here.

## Explicitly out of scope

- Formatting of comments — that is 013.
- Comment language — that is 011.

## Outcome

- The rule already lived in `CLAUDE.md` (`## Comments: what earns one`); this
  task linked it from `tasks/README.md` and `sapi/fpmng/README.md` and added a
  regression test so the contract and links cannot drift silently.
- `build/test-comment-content-rule.sh` checks the Keep/Delete/Never wording,
  the no-scheduled-cleanup sentence, and both README anchors. Wired into CI as
  a fast job (no php-src build).
- Not measured on the test box — documentation-only change.
