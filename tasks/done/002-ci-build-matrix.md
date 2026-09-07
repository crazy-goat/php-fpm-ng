# 002 — CI: a build matrix over the combinations that break silently

**Priority:** high, right after 001.
**Status:** done, scoped down — see Outcome.

## Context

There is no `.github/` in this repository. Nothing verifies a commit.

Two real defects found on 2026-09-06, both by accident while investigating
something else, and both of a kind a build matrix would have caught the day they
appeared:

- **A ZTS build does not compile.** `sapi/fpmng/fpm/fpm_pool_coop.c` uses bare
  `sapi_globals` and `output_globals` in 8 places. Those exist as variables only
  in a non-ZTS build; under ZTS they are macros. Discovered during an unrelated
  experiment (`docs/spike-tsrm-context.md`, Q5).
- **`build/static-full.sh` builds the wrong SAPI.** It configures
  `--enable-fpm --with-fpm-http` and copies `/build/sapi/fpm/php-fpm`, i.e. the
  old php-src proof of concept, not `sapi/fpmng`. `README.md` advertises a fully
  static `-static-pie` musl build running in `FROM scratch` as a headline
  feature; nothing checks it. See task 004.

Neither is a test failure. Both are build-matrix failures.

There is also a failure mode specific to this project's structure: we carry
patches against php-src in `patches/`, applied by `build/prepare.sh`. When
upstream moves, a patch stops applying. Today we find out only when a human
builds.

## Problem

Add continuous integration whose primary job is to prove that the tree still
*builds*, across the axes where breakage is silent, and that our php-src patches
still apply.

## Acceptance criteria

1. A CI configuration exists and runs on push and on pull request.
2. It pins a php-src revision (so a green run means something), runs
   `build/prepare.sh` against it, and **fails loudly if any patch in `patches/`
   does not apply**.
3. It builds at least this matrix, and a failure in any cell fails the run:
   - non-ZTS and **ZTS**
   - `session` built statically, built as a **shared module**, and **disabled**
   - **with** and **without** `libevent_openssl` (the TLS termination path in
     `sapi/fpmng/config.m4` is optional by design; the no-TLS build must keep
     working)
   - the fully static musl `-static-pie` build, of **`sapi/fpmng`** (see 004)
4. The ZTS cell may start as a known failure, but it must be recorded as an
   expected failure with a pointer to the reason — not silently excluded. When
   someone fixes it, CI is what tells us.
5. Warnings do not fail the build by default, but the run surfaces new warnings
   in a way a reviewer can see. `-Wall -Wextra` is already in use; one known
   benign warning is `-Wlogical-op` at `sapi/fpmng/fpm/fpm_pool_coop.c:426`
   (`errno == EAGAIN || errno == EWOULDBLOCK`, equal constants on Linux).
6. The `.phpt` suite from task 001 runs in CI once 001 has landed; until then CI
   is build-only and says so.

## Explicitly out of scope

- Deployment, releases, publishing images. "CD" is not part of this task.
- Performance benchmarking in CI. The numbers in `docs/` come from a dedicated
  box precisely because shared CI runners cannot produce comparable ones.
- Running anything that needs the shared test box at `192.168.8.50`.

## Open questions for whoever picks this up

- Where does CI run? The repository is private. If GitHub-hosted runners are
  used, note that a full php-src build is slow — decide whether the matrix runs
  on every push or only on pull requests, and say why in the config.
- How is the pinned php-src revision bumped, and by whom? An unpinned checkout
  makes every red build ambiguous: our regression, or upstream's change?

## Outcome — 2026-09-06

`.github/workflows/build-matrix.yml` landed (PR #1, merged as `3e5c375`).
Decisions made explicitly, not left as guesses:

- **Runner:** GitHub-hosted (repo is private but small; a self-hosted runner
  wasn't judged worth the upkeep yet).
- **Trigger:** `pull_request` only, not every push — a full php-src build is
  slow, and only a PR is a merge candidate.
- **php-src pin:** tag `php-8.5.9` (a real release tag, per project
  convention of pinning by tag, not branch — `docs/NOTES.md:2760`).

Delivered, matching acceptance criteria 1, 2 and 6:

- `patches` job: fails loudly if `patches/*.patch` stops applying to the
  pinned tag (build/prepare.sh's own exit code).
- `build` job: builds `sapi/fpmng` and `cli` and verifies the resulting
  binary.
- `phpt` job: runs the upstream FPM `.phpt` suite restored by task 001
  against that build. Final measured result: 123 PASS / 0 FAIL / 17 SKIP /
  1 WARN out of 141 discovered tests (the WARN is the same upstream
  XFAIL-passed case task 001 already documented in
  `docs/fpm-phpt-results.md`).

**Deliberately not delivered, deferred to release time** (acceptance
criteria 3, 4, 5 — not satisfied by this workflow, and the workflow's own
comments say so):

- The ZTS / session (static, shared, disabled) / TLS (with, without
  `libevent_openssl`) build matrix. Currently a single cell (non-ZTS,
  session static, TLS on). ~13 parallel php-src compiles per PR wasn't
  judged worth it before there's a release to protect.
- The fully static musl `-static-pie` build of `sapi/fpmng` (a corrected,
  separate invocation from the still-broken `build/static-full.sh` — see
  task 004, untouched here).
- Surfacing new `-Wall -Wextra` warnings to a reviewer.

Two real bugs were found and fixed along the way, surfaced specifically by
running task 001's phpt harness against an actual release tag instead of
the dev snapshot (php-src commit `67d1476d4d`, PHP 8.5.11-dev) task 001 was
validated against — the two versions' `sapi/fpm/tests/tester.inc` differ:

- `build/run-fpm-phpt.sh` created a `$HARNESS_DIR/fpm/php-fpm` symlink for
  `tester.inc::findExecutable()`'s directory-relative lookup, but never
  routed `TEST_PHP_EXECUTABLE` through that same harness directory — so the
  lookup never matched. Fixed by symlinking `TEST_PHP_EXECUTABLE` into the
  harness dir too.
- `gh12621.phpt` reads a source-tree fixture
  (`ext/standard/tests/misc/browscap.ini`) via a relative path; the CI
  job's flattened build artifact didn't include it, so the test failed on
  a missing file rather than on any real fpmng behavior. Fixed by staging
  that one fixture file.
- `http-basic.phpt`, task 001's one documented "intended difference,"
  doesn't exist in php-8.5.9's copy of `sapi/fpm/tests` at all — 150 tests
  discovered against the dev snapshot, 141 against this tag. Nothing to
  reclassify for it here; it's simply a different upstream snapshot.

Before the first release: re-add the full matrix and the static-musl job
(both already sketched and then deliberately removed from the workflow —
see its git history), and decide how to surface new compiler warnings.
