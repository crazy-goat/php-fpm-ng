# php-fpm-ng — workflow and working rules

A separate SAPI in `sapi/fpmng/`, built alongside upstream `sapi/fpm/`.
**Not a fork of php-src.** Target: small projects on a single VPS — one binary
plus application code in a container image, no nginx, no supervisord, no system
cron, no shell. One configuration file describes the application together with
its workers and crons.

## Language: English, everywhere

Decided 2026-09-06. English is the only language in this project:

- code comments in our own files
- `docs/`, `README.md`, `sapi/fpmng/README.md`
- GitHub issues, issue comments and pull requests
- commit messages
- log messages, error messages and configuration documentation

Two exceptions, both about files we do not own:

- Comments inherited from upstream in files that `build/prepare.sh` copies from
  `sapi/fpm/` are left alone. That script re-copies them on every run, so any
  edit there is lost anyway.
- Quoted material stays verbatim: error strings, log lines, command output,
  measurements. Never translate something that appears in the output of a
  program.

Existing Polish text is translated incrementally, file by file, one commit per
file — see task 012 in [`docs/task-archive.md`](docs/task-archive.md). Do not
bulk-translate.

## Comments: what earns one

Comments in this codebase are the primary record of **why**, and most of what
they record cannot be recovered from the code.

- **Keep** a comment that justifies a decision, cites a measurement, names a
  rejected alternative and the reason it was rejected, or warns about a trap the
  code cannot express.
- **Delete** a comment that restates the line below it.
- **Never** delete a comment containing a number, an error message, or a
  `file:line` reference without independently re-establishing the fact first.

There is no scheduled comment-cleanup pass. Redundant comments go away
opportunistically, in the same commit as a real change to that code.

## Architecture contract

- **Never open a PR against upstream php/php-src.**
- New behaviour goes into **new files** under `sapi/fpmng/fpm/`. Existing
  php-src files get minimal hooks only.
- Per-pool-type behaviour is a **field, a callback, or data** in
  `fpm_pool_type_s` (`sapi/fpmng/fpm/fpm_pool_type.h`). Never
  `if (type == ...)`, never `strcmp(type->name, ...)`.
- `sapi/fpmng/config.m4` is not hand-edited in its source-list part:
  `build/prepare.sh` substitutes `@FPMNG_SOURCES@` from `find fpm -name '*.c'`.
  An existing build directory has a frozen object list and will not see a new
  `.c` file — `buildconf --force` plus `config.nice` is required. The script
  warns about this at the end of its output; read that warning.
- Patches against php-src live in `patches/` and are applied by `prepare.sh`.
  Anything that must change in core belongs there, gated so that upstream
  behaviour is the default.

## Evidence

- Claims about behaviour are backed by a `file:line`, a measurement, or a named
  document. A guess is written as a guess.
- Report what was actually measured. "Not measured" is a complete answer and is
  preferred over an estimate presented as a result.
- Before measuring, confirm you are measuring the binary you think you are
  (`strings` on a distinctive literal). This project has already drawn a false
  conclusion from measuring someone else's build.

## Test box

`192.168.8.50`, user `piotr`, passwordless sudo. It is **shared** — other work
runs there concurrently.

**Manual builds and benchmarks only. Never CI.** It ran the CI matrix until
2026-09-12; the runners were then unregistered and their services disabled,
because this repository is public and a self-hosted runner on a public
repository is a machine a pull request from a stranger can be made to execute
code on. Re-registering one is a decision, not a convenience.

- Work in your own directory and your own port range.
- Never `pkill php-fpm` or anything matching by binary name. Kill by port
  (`ss -lntp`) or by the pid file of your own pool.
- MySQL (3306) and Redis (6379) are shared: use your own database and Redis
  index, never `FLUSHALL`.
- The Laravel framework runner's negative controls are *designed* to corrupt
  their database (empty static lists). Under `SERVICE_MODE=external` they are
  skipped (issue #51); run them with `SERVICE_MODE=docker`, or set
  `LARAVEL_NEGATIVE_ALLOW_EXTERNAL=1` only for a private MySQL/Redis.
- `pgrep -f "<pattern>"` matches its own command line. Do not use it to wait for
  a job to finish.

## Work tracking

Open work lives in **GitHub Issues** (`gh issue list`). There is no task
directory in the tree any more; the file-based tracker that used to live in
`tasks/` was migrated on 2026-09-08 and is indexed in
[`docs/task-archive.md`](docs/task-archive.md), which is also how you turn an
old `task NNN` reference in a comment or a commit message into something
readable.

What an issue must contain — the rules the file tracker had, unchanged, because
they are what made those files worth reading:

- **What** and **why**, never **how**. No patches, no prescribed function names.
  Whoever picks it up decides the implementation.
- Every claim about current behaviour is traceable: a `file:line`, a measured
  number, or a named document. A guess is written as a guess.
- Acceptance criteria checkable by someone who did not write the issue. "Works
  correctly" is not a criterion; "8/8 concurrent requests return their own
  session id" is.
- What is explicitly **out of scope**, when the boundary is not obvious.

Labels carry what the file headers used to:

- type — `bug`, `enhancement`, `spike`, `refactor`, `decision`, `build`, `test`,
  `measurement`, `documentation`, `epic`
- area — `area:http-direct`, `area:http-gateway`, `area:worker`, `area:tls`,
  `area:acme`, `area:fiber`, `area:ci`, `area:pool-types`, `area:test-harness`
- `priority:high` / `priority:medium` / `priority:low`
- `track:nice-to-have` — work that only applies to `pool.executor = fiber`,
  which is behind a build flag that is **off by default**, so a stock binary
  does not contain that code. Those priorities rank against **each other**, not
  against the main line.

Out-of-scope discoveries while coding go in `findings.md` at the repo root
(gitignored) and are promoted to issues, not kept there — see step 3.

---

# Issue workflow

Step-by-step process for picking up an issue and landing it on `main`.

## 1. Read the issue

1. `gh issue view <N>` (`gh issue list --label area:http-direct` to find one).
2. Confirm acceptance criteria are checkable and note what is explicitly out of
   scope. If they are not checkable, fix the issue first — a comment or an edit
   — before writing code.
3. Trace any `file:line` or document references before writing code. An issue
   migrated from the old tracker may cite `task NNN`; resolve it through
   [`docs/task-archive.md`](docs/task-archive.md).
4. Assign yourself, so two worktrees do not start the same work.

## 2. Isolated worktree (always)

Never implement on a dirty `main`. Always branch in a dedicated worktree:

```sh
git fetch origin main
git worktree add ../php-fpm-ng-worktrees/issue-<N>-<slug> -b issue/<N>-<slug> origin/main
cd ../php-fpm-ng-worktrees/issue-<N>-<slug>
```

Use a unique directory name (issue number + short slug + date if needed). One
issue per worktree. Branches from before the migration are named
`task/<NNN>-<slug>`; that prefix is retired for new work.

## 3. Implement

- New behaviour in new files under `sapi/fpmng/fpm/`; minimal hooks elsewhere.
- Per-pool-type behaviour via `fpm_pool_type_s` fields/callbacks — never
  `if (type == ...)`.
- Comments explain **why** — see [Comments: what earns one](#comments-what-earns-one)
  above.
- After adding a `.c` file: `buildconf --force` + reconfigure in the build tree
  (see `build/prepare.sh` warning).

### Findings during coding

While working, jot down follow-ups worth doing but out of scope for the current
issue in **`findings.md`** at the repo root (or in the worktree — same file
name). This file is **gitignored**; it is a personal scratch pad, not part of
the tree.

Format (keep it short):

```markdown
## YYYY-MM-DD — issue #N

- **area:** one-line description
  **why:** what you saw (`file:line` or measurement)
  **suggested issue:** one sentence, no implementation
```

An entry that survives review (step 5) is opened as an issue and removed from
`findings.md`. The file is a scratch pad between the observation and the issue,
never a backlog of its own.

Do not fix unrelated problems in the same PR unless the issue explicitly asks for
it. Record them in `findings.md` instead.

## 4. Build and test

Prefer the **test box** (“poligon”, `192.168.8.50`, user `piotr`) when it is
reachable. Fall back to a local build when it is not.

### On the test box (preferred)

The box is **shared**. Other work runs concurrently.

- Use your own scratch directory, e.g. `~/rd/task-<NNN>-<date>/`.
- Use your own port range; never `pkill php-fpm` or kill by binary name — kill
  by port (`ss -lntp`) or your pool’s pid file.
- MySQL (3306) and Redis (6379) are shared: own database / Redis DB index, never
  `FLUSHALL`.
- The Laravel framework runner's negative controls corrupt their database by
  design; under `SERVICE_MODE=external` they are skipped (issue #51). Run them
  with `SERVICE_MODE=docker`, or set `LARAVEL_NEGATIVE_ALLOW_EXTERNAL=1` only
  for a private MySQL/Redis.
- Confirm the binary under test with `strings` on a distinctive literal before
  measuring.
- **Clean up** when done: stop your processes, remove temp dirs you created.

Typical build path on the box:

```sh
# on 192.168.8.50, in your scratch dir
git clone /path/to/repo-or-fetch-from-origin .
./build/prepare.sh "$PWD/php-src"
cd php-src && ./buildconf --force && ./configure ... && make -j"$(nproc)" fpmng cli
# run tests / manual checks, then rm -rf the scratch dir if throwaway
```

### Locally (fallback)

When the test box is unavailable, build and run tests on the dev machine using
the same `build/prepare.sh` flow. CI still remains the authoritative merge gate
for full matrix coverage.

### Tests

- Add regression tests that match the issue (`.phpt`, shell harness under
  `build/test-*.sh`, etc.) when the behaviour is checkable.
- Run relevant suites before opening the PR (`build/run-fpmng-phpt.sh`,
  task-specific scripts, fast doc checks like `build/test-comment-content-rule.sh`).
- Wire new checks into `.github/workflows/build-matrix.yml` when appropriate.

## 5. Code review (Bugbot subagent)

Before opening the PR, run a **Bugbot** review on **added/changed code only**.
Scope is deliberately narrow:

- **Major issues only** — correctness, security, resource leaks, broken
  invariants, clear logic errors. Not style nits, naming bikesheds, or
  hypothetical edge cases unless they can plausibly bite in production.
- **Diff scope:** branch changes against `main` (default), not the whole tree.

Launch one `bugbot` subagent with:

```text
Full Repository Path: <absolute path to the issue worktree>
Diff: branch changes
Custom Instructions: Review only added/changed code. Report major issues only
(correctness, security, leaks, broken invariants). Skip style, formatting, and
minor nits. Also read findings.md in the repo root: for each entry, say whether
it warrants its own issue and why. Do not invent work for trivial or
duplicate items.
```

Act on review findings that are clearly valid before opening the PR. If Bugbot
confirms a `findings.md` item is substantive, **open an issue** for it
(`gh issue create`, with labels) and drop the entry from `findings.md`. Do not
bundle the follow-up into the current PR unless the user asks.

## 6. Record the outcome on the issue

The `Outcome` section the old task files carried is not paperwork — it was the
only record of what had actually been measured. It now lives as a comment on the
issue, written when the PR is ready and not from memory a week later:

- what was done,
- what was **measured** (a number, or "not measured" — that is a complete
  answer),
- what was deliberately left out, and whether it needs an issue of its own.

## 7. Pull request (always)

All changes land through a PR — never push directly to `main`.

```sh
git add ...
git commit -m "..."
git push -u origin issue/<N>-<slug>
gh pr create --title "..." --body "Closes #<N>

..."
```

PR body: `Closes #<N>` first, so the merge closes the issue; then a short
summary, a test plan checklist, and a note if anything was “not measured”.

## 8. CI and merge

- Watch checks: `gh pr checks <number> --watch`.
- Merge when required jobs are green (`gh pr merge <number> --merge --delete-branch`).
- `lint` may be non-blocking (`continue-on-error: true`); do not treat a green
  lint job as “zero findings” without reading the artifact if you touched C.

## 9. Clean up (always)

Back on the primary checkout:

```sh
cd /path/to/php-fpm-ng          # main worktree
git pull origin main
git worktree remove ../php-fpm-ng-worktrees/issue-<N>-<slug>
git branch -d issue/<N>-<slug>   # if not deleted by merge
```

Confirm the issue actually closed (`gh issue view <N>`) — `Closes #<N>` only
fires on merge into the default branch — and post the outcome comment from step
6 if it is not there yet.

If merge deleted the remote branch but the local worktree remains, remove the
worktree directory manually. Leave `findings.md` in place locally (gitignored) —
trim entries that became issues or were rejected.

## Checklist (copy for each issue)

- [ ] Issue read and assigned; acceptance criteria understood
- [ ] Worktree + branch `issue/<N>-<slug>`
- [ ] Implementation + tests; English throughout
- [ ] Build/test on poligon if available, else local
- [ ] Poligon: own dirs/ports; cleaned up after
- [ ] `findings.md` updated for out-of-scope discoveries
- [ ] Bugbot review (major issues only); valid findings → new issues
- [ ] Outcome written up (done / measured / left out)
- [ ] PR opened with `Closes #<N>`; CI green
- [ ] Merged; issue closed; worktree removed; `main` pulled
