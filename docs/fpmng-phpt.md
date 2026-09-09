# php-fpm-ng PHPT tests

Regression tests owned by this repository live under `sapi/fpmng/tests/` as
`fpmng-*.phpt` files. Every `.phpt` file in that directory is ours; the prefix
is how the runner finds them, and `run-fpmng-phpt.sh` fails when a file this
repo owns is not reached by it (issue #95). They use the upstream FPM harness copied by
`build/prepare.sh` (`tester.inc`, `skipif.inc`, FastCGI client helpers) and
assert behaviour that upstream's 150-test suite does not cover: `pool.type`,
`pool.executor`, the HTTP gateway, cron, supervisor, and the status pool.

Each test's `--TEST--` title and inline comments name the documented claim they
protect (`docs/NOTES.md`, `docs/cron.md`, `docs/frameworks.md`, or a finished
task file).

## Prerequisites

Same as [`fpm-phpt.md`](fpm-phpt.md): a prepared php-src tree, matching patch
stack, built `php-fpm-ng` and CLI `php` from that tree, `strings`, and a
SHA-256 tool. The fiber isolation and fiber matrix tests require a binary
configured with `--enable-fpmng-fiber`; without it they skip cleanly via
`--SKIPIF--`.

## Run only the fpmng-owned suite

```sh
REPO=/path/to/php-fpm-ng
"$REPO/build/prepare.sh" /path/to/php-src
cd /path/to/php-build
./buildconf --force && ./config.nice && make fpmng cli

TEST_PHP_EXECUTABLE=/path/to/php-build/sapi/cli/php \
TEST_PHP_FPM_EXECUTABLE=/path/to/php-build/sapi/fpmng/php-fpm-ng \
TEST_FPM_TIMEOUT=120 \
"$REPO/build/run-fpmng-phpt.sh" \
    /path/to/php-src \
    /path/to/dedicated/fpmng-phpt-results
```

`TEST_FPM_TIMEOUT` defaults to **120** seconds in this runner because
`fpmng-cron-schedule.phpt` waits for the next minute boundary.

## Result files

Identical layout to the upstream runner (`discovered.tsv`, `results.tsv`,
`summary.txt`, `metadata.txt`, logs). Categories are the same strict mapping
documented in `fpm-phpt.md`.

## Excluding a test from the runner

`sapi/fpmng/tests/not-run-in-ci.list` holds, one per line, a test file name
followed by the reason it cannot run where the runner runs. The runner drops
those from the run and rejects a name with no reason, a name with no file, and
a name without the `fpmng-` prefix — an excluded test keeps the prefix, because
`run-fpm-phpt.sh` reads "not named `fpmng-*`" as "upstream's" and would run it
in the other job. The list is empty today. It is not an escape hatch for a
failing test — that gets fixed or gets an issue.

## CI

The `fpmng-phpt` job in `.github/workflows/build-matrix.yml` downloads the
canonical build artifact and runs this runner. It is the only job that runs
these tests: before issue #95 the `phpt` job ran them a second time, because
that runner swept the whole copied test directory.
