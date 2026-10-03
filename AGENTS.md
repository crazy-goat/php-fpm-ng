# AGENTS.md

Project commands and specifics for php-fpm-ng. The development process (issue,
worktree, review, PR, merge) is in [docs/workflow.md](docs/workflow.md), the release
process in [docs/release-workflow.md](docs/release-workflow.md). The default branch is `main`.

php-fpm-ng is a separate SAPI in `sapi/fpmng/`, built alongside upstream `sapi/fpm/`.
**It is not a fork of php-src.** Target: small projects on a single VPS, with one binary plus
application code in a container image, and no nginx, no supervisord, no system cron and no
shell. One configuration file describes the application together with its workers and crons.

## Language: English, everywhere

English is the only language in this project (decided 2026-09-06):

- code comments in our own files
- `docs/`, `README.md`, `sapi/fpmng/README.md`
- GitHub issues, issue comments and pull requests
- commit messages
- log messages, error messages and configuration documentation

Two exceptions, both about files we do not own:

- Comments inherited from upstream in the vendored copies under `third_party/php-src/` are
  left alone: `build/vendor-php-src.sh check` refuses any edit made to them. The same holds
  for files that the retained from-source tool `build/prepare.sh` copies from `sapi/fpm/`,
  which it re-copies on every run.
- Quoted material stays verbatim: error strings, log lines, command output, measurements.
  Never translate something that appears in the output of a program.

Existing Polish text is translated incrementally, file by file, one commit per file (see
task 012 in [`docs/task-archive.md`](docs/task-archive.md)). Do not bulk-translate.

## Comments: what earns one

Comments in this codebase are the primary record of **why**, and most of what they record
cannot be recovered from the code.

- **Keep** a comment that justifies a decision, cites a measurement, names a rejected
  alternative and the reason it was rejected, or warns about a trap the code cannot express.
- **Delete** a comment that restates the line below it.
- **Never** delete a comment containing a number, an error message, or a `file:line`
  reference without independently re-establishing the fact first.

There is no scheduled comment-cleanup pass. Redundant comments go away opportunistically, in
the same commit as a real change to that code. `build/test-comment-content-rule.sh` checks
that this section and the links to it stay in place.

## Architecture contract

- **Never open a PR against upstream php/php-src.**
- New behaviour goes into **new files** under `sapi/fpmng/fpm/`. Existing php-src files get
  minimal hooks only.
- Per-pool-type behaviour is a **field, a callback, or data** in `fpm_pool_type_s`
  (`sapi/fpmng/fpm/fpm_pool_type.h`). Never `if (type == ...)`, never
  `strcmp(type->name, ...)`.
- The packages and CI build with `build/libphp-build.sh` against the distribution's PHP 8.5
  SDK; it globs `sapi/fpmng/fpm/*.c`, so a new `.c` file is picked up with no extra step.
  `sapi/fpmng/config.m4` belongs only to the retained from-source flow (`build/prepare.sh`,
  macOS development): its source list is substituted from `find fpm -name '*.c'`, and an
  existing build directory has a frozen object list (`buildconf --force` plus `config.nice`).
- `main` carries no php-src patch (#592): `patches/` holds only history notes,
  `build/prepare.sh` touches nothing outside `sapi/fpmng/` and `ext/fpmng_metrics/`, and
  `third_party/php-src/` holds pristine, hash-checked copies of the pinned tag. The patch stack
  that changes `libphp` (fibers, async) lives only on branch `async`. `main` needs nothing
  from the engine that a distribution `libphp` does not export.

## Evidence

- Claims about behaviour are backed by a `file:line`, a measurement, or a named document. A
  guess is written as a guess.
- Report what was actually measured. "Not measured" is a complete answer and is preferred
  over an estimate presented as a result.
- Before measuring, confirm you are measuring the binary you think you are (`strings` on a
  distinctive literal). This project has already drawn a false conclusion from measuring
  someone else's build.

## Layout

| Path | Content |
|---|---|
| `sapi/fpmng/` | The SAPI: `fpm/` (C sources), `acme/` (PHP ACME client), `tests/` (`fpmng-*.phpt`) |
| `ext/fpmng_metrics/` | Metrics extension (`fpm_metric_register/inc/set/observe`) |
| `third_party/php-src/` | Vendored php-src subset (FastCGI layer, upstream FPM files, test fixtures), see its README |
| `patches/` | Notes on the php-src patches `main` dropped; `main` carries none, patches live on branch `async` |
| `build/` | Build, package, lint and test scripts |
| `docker/`, `.github/docker/` | Dockerfiles (minimal package image, package-gate images) |
| `packaging/` | `.deb` and `.apk` configuration and service files |
| `examples/` | Runnable examples (each has its own Dockerfile or compose file) |
| `tests/frameworks/` | Slim 4 smoke test on `gateway`+`fastcgi` and `http-direct` classic (fiber probes: `async/tests/frameworks/` on branch `async`) |
| `docs/` | Design notes, spike reports, process docs |

## Commands

The build needs Linux and the PHP 8.5 NTS SDK of the distribution (Ubuntu 26.04 with
`php8.5-dev libphp8.5-embed php8.5-cli libevent-dev libssl-dev libacl1-dev`, or Alpine edge).
No php-src checkout is needed.

```bash
# Build (Linux). Toggles: FPMNG_TLS, FPMNG_ACME, FPMNG_DEBUG_CLOCK (CI sets all three to 1).
FPMNG_TLS=1 FPMNG_ACME=1 FPMNG_DEBUG_CLOCK=1 ./build/libphp-build.sh "$PWD/out"

# Lint: clang-tidy (build/lint-c.sh) + clang-format + shellcheck + hadolint.
# Check only, runs every step; --fix applies clang-format first. A missing tool fails the run.
# clang-tidy needs the output directory of a libphp-build.sh run for its compile flags:
FPMNG_LIBPHP_OUT="$PWD/out" bin/lint.sh
FPMNG_LIBPHP_OUT="$PWD/out" bin/lint.sh --fix

# Tests: the fpmng-owned .phpt suite, then the upstream FPM .phpt suite
TEST_PHP_EXECUTABLE="$(php-config8.5 --php-binary)" \
TEST_PHP_FPM_EXECUTABLE="$PWD/out/php-fpm-ng" \
TEST_FPM_EXTENSION_DIR="$(php-config8.5 --extension-dir)" \
TEST_FPM_TIMEOUT=120 \
./build/run-fpmng-phpt.sh - "$PWD/fpmng-phpt-results"
./build/run-fpm-phpt.sh - "$PWD/phpt-results"      # same environment, without the last two

# Hermetic checks (no build product needed)
./build/test-comment-content-rule.sh
./build/test-fpmng-phpt-coverage.sh
./build/vendor-php-src.sh check
./build/test-libphp-build-refusals.sh
./build/test-phpt-tree.sh
./build/test-package-gate-expected.sh
./build/test-shipped-configs.sh static   # `images <binary>` needs docker (CI job `examples`)
./build/test-libphp-abi-guard.sh      # needs the SDK and a compiler

# Slim 4 framework smoke test (needs Composer or network for the pinned phar, and php-curl)
FPMNG="$PWD/out/php-fpm-ng" PHP="$(php-config8.5 --php-binary)" tests/frameworks/slim4/bin/run.sh

# Shell scenarios against the built binary (see .github/workflows/build-matrix.yml)
./build/test-http-tls-reload.sh "$PWD/out/php-fpm-ng"
```

On macOS use the retained from-source flow (`build/prepare.sh`), or run the Linux commands in
an `ubuntu:26.04` container after `build/ci-install-deps.sh build` (needs root). php-fpm
refuses to start as root, so run the suites as an unprivileged user; a run whose log shows
SKIP counts instead of PASS counts did not test anything. CI is the authoritative merge gate.

`bin/lint.sh` details:

- clang-format 23 (CI pins `clang-format==23.1.2`) on our own C sources, style in
  `.clang-format`. Files listed in `build/clang-format-exclude.txt` are not checked yet (they
  would need a large whitespace-only reformat); shrink that list, never grow it. The nine
  files that are modified copies of upstream `sapi/fpm/` files are excluded for good, so they
  stay diffable against upstream. `third_party/` is vendored and excluded.
- shellcheck v0.11.0 at its default (style) severity on every tracked shell script (also
  extensionless ones with a shell shebang). Put the reason on the line above
  `# shellcheck disable=SCxxxx`; no blanket disable in a `.shellcheckrc`.
- hadolint v2.12.0 on every tracked Dockerfile. Put the reason on the line above
  `# hadolint ignore=DLxxxx`.

## Test box

`192.168.8.50`, user `piotr`, passwordless sudo. It is **shared**, other work runs there
concurrently.

**Manual builds and benchmarks only. Never CI.** It ran the CI matrix until 2026-09-12; the
runners were then unregistered and their services disabled, because this repository is public
and a self-hosted runner on a public repository is a machine a pull request from a stranger
can be made to execute code on. Re-registering one is a decision, not a convenience.

- Work in your own directory (for example `~/rd/<issue>-<date>/`) and your own port range.
- Never `pkill php-fpm` or anything matching by binary name. Kill by port (`ss -lntp`) or by
  the pid file of your own pool.
- MySQL (3306) and Redis (6379) are shared: use your own database and Redis index, never
  `FLUSHALL`.
- `pgrep -f "<pattern>"` matches its own command line. Do not use it to wait for a job to
  finish.
- Typical build path on the box (Ubuntu 26.04 with the SDK packages above; no php-src
  checkout): clone the repository, run the build and the test commands from "Commands"
  with `$PWD/out`, then stop your processes and remove the temporary directories.

## Work tracking

Open work lives in **GitHub Issues** (`gh issue list`). There is no task directory in the
tree; the file-based tracker that used to live in `tasks/` was migrated on 2026-09-08 and is
indexed in [`docs/task-archive.md`](docs/task-archive.md), which is also how you turn an old
`task NNN` reference in a comment or a commit message into something readable.

What an issue must contain:

- **What** and **why**, never **how**. No patches, no prescribed function names. Whoever
  picks it up decides the implementation.
- Every claim about current behaviour is traceable: a `file:line`, a measured number, or a
  named document. A guess is written as a guess.
- Acceptance criteria checkable by someone who did not write the issue. "Works correctly" is
  not a criterion; "8/8 concurrent requests return their own session id" is.
- What is explicitly **out of scope**, when the boundary is not obvious.

Labels are the organization's standard ones (`type:*`, `priority:*`, `status:*`), plus:

- area: `area:http-direct`, `area:http-gateway`, `area:worker`, `area:tls`, `area:acme`,
  `area:fiber`, `area:ci`, `area:pool-types`, `area:test-harness`
- kind: `spike`, `decision`, `measurement`, `epic`

Work that only applies to `pool.executor = fiber` (not on `main`; branch `async`) ranks against other such work, not against the main line.

## Worktree notes

- `bin/worktree.sh` picks the worktree location (see [docs/workflow.md](docs/workflow.md),
  step 2); `--dir <path>` sets it. Branches from
  before the migration are named `task/<NNN>-<slug>` or `issue/<N>-<slug>`; new work uses
  `<type>/issue-<N>-<slug>`.
- `bin/worktree-setup.sh` only reports missing tools; the project has no package
  dependencies to install.
- The compose files (`examples/*/compose.yaml`) publish
  ports as `${..._PORT:-N}` and set no `container_name`, so worktrees do not collide. The
  harnesses read the ports from the environment (`.env.worktree`).
- After adding a `.c` file nothing is needed for `build/libphp-build.sh`; only a from-source
  `prepare.sh` build tree needs `buildconf --force` and a reconfigure.
- Wire a new check into `.github/workflows/build-matrix.yml`. Jobs run in a stock
  `ubuntu:26.04` container, install the SDK with `build/ci-install-deps.sh <role>` and run
  their steps as the unprivileged `ci` user.

## Review checklist (project specific)

- [ ] New behaviour is in new files; per-pool-type behaviour is data in `fpm_pool_type_s`
- [ ] Comments explain **why**; no comment with a number, error message or `file:line` was
      deleted without re-establishing it
- [ ] Claims in docs and comments carry a `file:line`, a measurement or "not measured"
- [ ] A regression test (`.phpt`, or a `build/test-*.sh` scenario wired into CI) exists
- [ ] Built and tested on Linux, or the reason it was not is stated in the PR
- [ ] `bin/lint.sh` passes; `CHANGELOG.md` has an entry under `[Unreleased]`
- [ ] The test box, if used, was left clean (own directories and ports only)
