# 023 — Reload signalling: the comment does not cover the `status` pool, and the README's churn note is stale

**Priority:** low. Two small documentation defects in recently added code.
**Status:** done.

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

## Outcome

- `sapi/fpmng/fpm/fpm_process_ctl.c:165` — rewrote the comment to cover all
  three `serves_requests == 0` pool types and distinguish the two reasons:
  `supervisor`/`cron` get `SIGTERM` to finish their current iteration;
  `status` gets it because it has no work to finish and would otherwise wait
  out the master's `SIGQUIT`→`SIGTERM` escalation for nothing. Cross-refs
  `fpm_pool_status.c:435-439` and `:443-446`.
- `sapi/fpmng/fpm/fpm_pool_status.c:435` — updated the "accepted delay" note:
  it no longer applies on reload (fpm_process_ctl.c now sends `SIGTERM`
  directly, see NOTES 3x), only to an explicit graceful stop or log rotation,
  which still use the ordinary `SIGQUIT` fan-out.
- `sapi/fpmng/README.md` — the `fpm_process_ctl.c` row now states status's
  distinct reason instead of implying it also "finishes an iteration"; the
  churn-list sentence now says explicitly why this already-taken-over file
  isn't on it (small, self-contained change, not ongoing churn).
- `docs/NOTES.md:2163` (status task's "Not done/uncertain" list) carried the
  same now-stale claim that the escalation delay is unconditionally accepted;
  added a cross-reference to 3x noting reload no longer has this delay.
- No behaviour changed — this was comments and docs only, so no new test was
  added. Verified by reading the edited comments against the actual line
  numbers they cite (`grep -n`) rather than assuming they'd stay accurate
  after the edits grew the comment blocks.
- Left out: did not rename or renumber the duplicate `## 3x.` heading in
  `docs/NOTES.md` (there is an unrelated, pre-existing `3x` about Fiber DNS at
  line 3375) — out of scope for this task.
