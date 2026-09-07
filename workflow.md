# php-fpm-ng — workflow and working rules

A separate SAPI in `sapi/fpmng/`, built alongside upstream `sapi/fpm/`.
**Not a fork of php-src.** Target: small projects on a single VPS — one binary
plus application code in a container image, no nginx, no supervisord, no system
cron, no shell. One configuration file describes the application together with
its workers and crons.

## Language: English, everywhere

Decided 2026-09-06. English is the only language in this project:

- code comments in our own files
- `docs/`, `README.md`, `sapi/fpmng/README.md`, `tasks/`
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
file — see `tasks/012`. Do not bulk-translate.

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

- Work in your own directory and your own port range.
- Never `pkill php-fpm` or anything matching by binary name. Kill by port
  (`ss -lntp`) or by the pid file of your own pool.
- MySQL (3306) and Redis (6379) are shared: use your own database and Redis
  index, never `FLUSHALL`.
- `pgrep -f "<pattern>"` matches its own command line. Do not use it to wait for
  a job to finish.

## Tasks

Open work is one file per task in `tasks/`, finished work moves to
`tasks/done/`. See `tasks/README.md`.

Out-of-scope discoveries while coding go in `findings.md` at the repo root
(gitignored).

---

# Task workflow

Step-by-step process for picking up a numbered task from `tasks/` and landing it
on `main`.

## 1. Read the task

1. Open `tasks/<NNN>-*.md` (or `tasks/nice-to-have/<NNN>-*.md`).
2. Confirm acceptance criteria are checkable and note what is explicitly out of
   scope.
3. Trace any `file:line` or document references before writing code.

## 2. Isolated worktree (always)

Never implement on a dirty `main`. Always branch in a dedicated worktree:

```sh
git fetch origin main
git worktree add ../php-fpm-ng-worktrees/task-<NNN>-<slug> -b task/<NNN>-<slug> origin/main
cd ../php-fpm-ng-worktrees/task-<NNN>-<slug>
```

Use a unique directory name (task number + short slug + date if needed). One task
per worktree.

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
task in **`findings.md`** at the repo root (or in the worktree — same file
name). This file is **gitignored**; it is a personal scratch pad, not part of
the tree.

Format (keep it short):

```markdown
## YYYY-MM-DD — task NNN

- **area:** one-line description
  **why:** what you saw (`file:line` or measurement)
  **suggested task:** one sentence, no implementation
```

Do not fix unrelated problems in the same PR unless the task explicitly asks for
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

- Add regression tests that match the task (`.phpt`, shell harness under
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
Full Repository Path: <absolute path to the task worktree>
Diff: branch changes
Custom Instructions: Review only added/changed code. Report major issues only
(correctness, security, leaks, broken invariants). Skip style, formatting, and
minor nits. Also read findings.md in the repo root: for each entry, say whether
it warrants a new task file under tasks/ and why. Do not invent tasks for
trivial or duplicate items.
```

Act on review findings that are clearly valid before opening the PR. If Bugbot
confirms a `findings.md` item is substantive, **create a task file** in
`tasks/` (separate commit or follow-up — not bundled into the current task’s PR
unless the user asks).

## 6. Finish the task file

In the **same commit** that completes the work:

1. Move `tasks/<NNN>-*.md` → `tasks/done/`.
2. Set `Status: done` and append an **Outcome** section: what was done, what was
   measured, what was left out.

## 7. Pull request (always)

All changes land through a PR — never push directly to `main`.

```sh
git add ...
git commit -m "..."
git push -u origin task/<NNN>-<slug>
gh pr create --title "..." --body "..."
```

PR body: short summary, test plan checklist, note if anything was “not measured”.

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
git worktree remove ../php-fpm-ng-worktrees/task-<NNN>-<slug>
git branch -d task/<NNN>-<slug>   # if not deleted by merge
```

If merge deleted the remote branch but the local worktree remains, remove the
worktree directory manually. Leave `findings.md` in place locally (gitignored) —
trim entries that became tasks or were rejected.

## Checklist (copy for each task)

- [ ] Task read; acceptance criteria understood
- [ ] Worktree + branch `task/<NNN>-<slug>`
- [ ] Implementation + tests; English throughout
- [ ] Build/test on poligon if available, else local
- [ ] Poligon: own dirs/ports; cleaned up after
- [ ] `findings.md` updated for out-of-scope discoveries
- [ ] Bugbot review (major issues only); valid findings → task files
- [ ] Task moved to `tasks/done/` with Outcome
- [ ] PR opened; CI green
- [ ] Merged; worktree removed; `main` pulled
