# tasks/

One file per unit of work. English only, even though most of the codebase and
`docs/` are still Polish (see `011-code-comment-language-policy.md` and
`012-translate-docs-to-english.md`).

**How to execute a task:** [`workflow.md`](../workflow.md) — worktree, poligon
or local build, tests, Bugbot review, PR, merge, cleanup. Scratch follow-ups
discovered while coding belong in `findings.md` at the repo root (gitignored).

## Tracks

- `tasks/` — the main line: the HTTP gateway, cron, proxying, packaging, CI
  and the tests. This is where the project is focused. ("The scheduler" used
  to appear here as a separate item; task 034 decided it names no capability
  `cron` doesn't already provide, so the word was retired — see
  `tasks/done/034-scheduler-define-or-drop.md`.)
- `tasks/nice-to-have/` — work that only applies to `pool.executor = fiber`.
  The fiber executor sits behind a build flag that is **off by default**, so a
  stock binary does not contain that code. These tasks keep their own
  `Priority:` line, which ranks them *against each other*, not against the main
  line.
- `tasks/done/` — finished, with an `Outcome` section (see the rules below).

Moving a task between tracks is a normal edit, not a decision that needs
ceremony: if fiber stops being optional, or a nice-to-have turns out to block
the main line, move the file and say why in the commit message.

## Rules

- A task describes **what** and **why**, never **how**. No code, no patches, no
  prescribed function names. Whoever picks it up decides the implementation.
- Every claim about current behaviour must be traceable: a `file:line`, a
  measured number, or a named document. If something is a guess, it is written
  as a guess.
- Acceptance criteria must be checkable by someone who did not write the task.
  "Works correctly" is not a criterion; "8/8 concurrent requests return their
  own session id" is.
- When a task is finished, move the file to `tasks/done/` in the same commit
  that finishes it, and append a short **Outcome** section: what was actually
  done, what was measured, what was left out.

## Conventions that apply to every task

- **Never open a PR against upstream php/php-src.** This project is a separate
  SAPI in `sapi/fpmng/`, not a fork.
- New behaviour goes into **new files** under `sapi/fpmng/fpm/`. Existing
  php-src files get minimal hooks only.
- Per-pool-type behaviour is added as a **field, a callback, or data** in
  `fpm_pool_type_s` (see `sapi/fpmng/fpm/fpm_pool_type.h`). Never
  `if (type == ...)` and never `strcmp(type->name, ...)`.
- `sapi/fpmng/config.m4` is not hand-edited in the source-list part:
  `build/prepare.sh` substitutes `@FPMNG_SOURCES@` from `find fpm -name '*.c'`.
  An existing build directory has a frozen object list and will not see a new
  `.c` file — `buildconf --force` plus `config.nice` is required. The script
  warns about this at the end of its output.
- The test box (`192.168.8.50`, user `piotr`, passwordless sudo) is shared.
  Work in your own directory, use your own port range, and never `pkill php-fpm`.
- Comment content — what earns a comment, what to delete, and why there is no
  bulk comment-cleanup pass — is in
  [`workflow.md`](../workflow.md#comments-what-earns-one) (task 014). Do not
  prune comments by apparent redundancy.
