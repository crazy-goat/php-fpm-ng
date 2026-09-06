# 023 — Reload signalling: the comment does not cover the `status` pool, and the README's churn note is stale

**Priority:** low. Two small documentation defects in recently added code.
**Status:** open.

## Context

`sapi/fpmng/fpm/fpm_process_ctl.c` was taken over from upstream so that a
graceful reload can send `SIGTERM` to long-lived pool types instead of `SIGQUIT`.
The change itself is small (about 24 lines against upstream) and respects the
pool-type contract: it selects on `type->serves_requests` and `type->child_main`,
never on the type's name.

### Defect 1 — the comment explains only two of the three affected types

The comment says request workers get `SIGQUIT` so the current request can drain,
while long-lived pool types have their own `SIGTERM` handler that finishes the
current script and prevents another iteration. That is accurate for `supervisor`
and `cron`.

It is **not** the reason for `status`, which also has `serves_requests = 0` and
therefore also now receives `SIGTERM`. `status` deliberately has no signal
handling at all — `sapi/fpmng/fpm/fpm_pool_status.c:434-442` explains that
`SIGTERM`'s default disposition is correct there because the pool has no work in
progress: every connection is fully served within one accept cycle.

So for `status` the change is beneficial for a different reason: the comment at
`fpm_pool_status.c:441` records that waiting for the master to escalate `SIGQUIT`
to `SIGTERM` was a known, accepted delay. Sending `SIGTERM` directly removes it.

As written, the comment leads a future reader to conclude that `status` has work
to finish. It does not.

### Defect 2 — `sapi/fpmng/README.md` contradicts itself

The file table now lists `fpm/fpm_process_ctl.c`, but the sentence under the
table still reads that `fpm_conf.c`, `fpm_status.c` and `fpm_main.c` will
eventually be taken over and are "the only FPM files with real upstream churn".
There is now a fourth taken-over file that the sentence does not account for.

## Problem

Fix both, and while doing so make the reload behaviour's rationale complete.

## Acceptance criteria

1. The comment in `fpm_process_ctl.c` covers all three affected pool types and
   distinguishes the two reasons: "let it finish its iteration"
   (`supervisor`, `cron`) versus "there is nothing to finish, so do not make it
   wait" (`status`).
2. `sapi/fpmng/README.md` is consistent: either `fpm_process_ctl.c` is added to
   the churn list, or the sentence is rewritten to say why it does not belong
   there (a plausible answer: it is a low-churn file, unlike the three named).
3. The accepted-delay note at `fpm_pool_status.c:441` is updated or
   cross-referenced, since the delay it describes no longer occurs on reload.

## Notes

- This is worth doing while the change is fresh. The reasoning is currently
  reconstructible only by reading three files and noticing that a fourth pool
  type falls into the same branch.
