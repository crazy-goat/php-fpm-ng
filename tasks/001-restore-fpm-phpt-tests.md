# 001 — Run upstream FPM's `.phpt` suite against `php-fpm-ng`

**Priority:** highest. Everything else in `tasks/` is easier to trust once this exists.
**Status:** open.

## Context

The project has roughly 14 000 lines of its own C code in `sapi/fpmng/fpm/` and
**zero automated tests**. There is no `.github/` directory either, so nothing is
verified except by hand on the test box.

The core value proposition of `php-fpm-ng` is that it *is* FPM plus extra pool
types — `build/prepare.sh` copies upstream `sapi/fpm/` and lays our files on
top. That claim is currently unverified: nothing checks that a pool with no
`pool.type` directive still behaves exactly like upstream FPM.

Upstream already ships the suite that would check it: `sapi/fpm/tests/` holds
**150 `.phpt` files** plus a harness (`tester.inc`). The harness picks its
binary like this:

    sapi/fpm/tests/tester.inc:228
    $phpPath = getenv("TEST_PHP_FPM_EXECUTABLE") ?: getenv("TEST_PHP_EXECUTABLE");

so it can be pointed at a differently-named binary without patching it.

`build/prepare.sh:30` currently deletes the copied tests outright:

    rm -rf "$PHPSRC/sapi/fpmng/tests"

with the comment *"Testy FPM odwoluja sie do binarki php-fpm, nie naszej. Wroca,
gdy beda wlasne."* — the intent was always to bring them back.

## Problem

Bring upstream's FPM test suite back and make it run against `php-fpm-ng`, so we
learn which upstream behaviours we have preserved and which we have broken.

The result of this task is **information**, not a green build. A test that fails
because `php-fpm-ng` genuinely differs from FPM is a finding to record, not a
thing to silence.

## Acceptance criteria

1. `build/prepare.sh` no longer unconditionally deletes the copied test
   directory, and the produced tree can run the suite.
2. There is a documented, single command that runs the suite against a built
   `php-fpm-ng` (including whatever environment variables it needs).
3. A results table is committed under `docs/`: for each of the 150 tests —
   pass, fail, or skipped — with the count for each category.
4. Every failure is triaged into exactly one of:
   - **our bug** — `php-fpm-ng` is wrong, upstream is right. Gets its own task file.
   - **intended difference** — we deliberately behave differently. Recorded with
     the reason and a pointer to where that decision is documented.
   - **test artefact** — the test hardcodes something about upstream's binary,
     paths, or build that has nothing to do with behaviour. Recorded with what
     exactly it assumes.
5. The triage of every failure is written down. A failure with no verdict is not
   an acceptable end state for this task.

## Explicitly out of scope

- Writing new tests for our own pool types and executors — that is task 003.
- Wiring any of this into CI — that is task 002. This task ends with something a
  human can run and a table of what it produced.
- Fixing the bugs the suite finds. File them; do not fix them here.

## Constraints

- The tests are copied from upstream by `prepare.sh`, like the rest of
  `sapi/fpm/`. Prefer solutions that keep them copied rather than forking 150
  files into this repo — a fork of the suite would drift and become worthless.
- If a test genuinely cannot work without modification, the modification belongs
  in our overlay, and the reason belongs in a comment.

## Notes for whoever picks this up

- The suite needs a real, working build. Build flags known to work on the test
  box are in `~/rd/a6/build/config.nice` (session, opcache, mysqlnd, openssl).
- Some FPM tests want to run as root or need specific users; `tester.inc` has
  `TEST_FPM_RUN_AS_ROOT` (`tester.inc:543`) and `TEST_FPM_EXTENSION_DIR`
  (`tester.inc:531`). Expect a non-trivial number of environment-dependent
  skips and report them as skips, not as passes.
