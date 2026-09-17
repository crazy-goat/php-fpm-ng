# php-fpm-ng PHPT tests

Regression tests owned by this repository live under `sapi/fpmng/tests/` as
`fpmng-*.phpt` files. Every `.phpt` file in that directory is ours; the prefix
is how the runner finds them, and `run-fpmng-phpt.sh` fails when a file this
repo owns is not reached by it (issue #95). They use the upstream FPM harness copied by
`build/prepare.sh` (`tester.inc`, `skipif.inc`, FastCGI client helpers) and
assert behaviour that upstream's 150-test suite does not cover: `pool.type`,
`pool.executor`, the HTTP gateway, cron, supervisor, and the operator
endpoint.

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

## Run one test, or a few

Any argument after the results directory is a filter (issue #141):

```sh
# one test, by name
"$REPO/build/run-fpmng-phpt.sh" /path/to/php-src /path/to/results \
    fpmng-cron-schedule.phpt

# a substring, and a glob — this selects both, once each
"$REPO/build/run-fpmng-phpt.sh" /path/to/php-src /path/to/results \
    cron-schedule 'fpmng-supervisor-*.phpt'
```

A filter is matched against the test's file name, as a shell glob and as a
plain substring, and every filter given must match at least one discovered
test — a typo stops the run instead of quietly reporting a green suite of
whatever else matched.

Go through the runner even for a single test rather than invoking
`run-tests.php` yourself: `FPM\Tester::findExecutable()`
(`sapi/fpmng/tests/tester.inc`, copied from php-src) never reads
`TEST_PHP_FPM_EXECUTABLE`. It looks for a binary named `php-fpm` two levels
above `TEST_PHP_EXECUTABLE`, so a direct invocation SKIPs with
`php-fpm binary not found` until the symlink harness the runner builds is
recreated by hand.

Filtering does not relax the ownership check: discovery and the coverage check
of issue #95 still see the whole owned suite, and an owned test the glob cannot
reach fails the run whatever the filter says. `metadata.txt` records both
counts, `discovered_tests` and `selected_tests`, plus the `filter` itself, so a
partial result directory cannot be mistaken for a full run.

## Result files

Identical layout to the upstream runner (`discovered.tsv`, `results.tsv`,
`summary.txt`, `metadata.txt`, logs), plus `selected.tsv` — the subset of
`discovered.tsv` that was actually handed to `run-tests.php`, equal to it when
no filter is given. `results.tsv` covers exactly the selected tests.
Categories are the same strict mapping documented in `fpm-phpt.md`.

`slow.tsv` is ours: two columns, seconds and test file, longest first, for every
test above `TEST_FPM_SHOW_SLOW_MS` (default 1000; `0` turns the table off). The
same table is echoed to stderr at the end of the run, so the CI log shows where
the time went without downloading the artifact. The suite is serial -- no `-j`,
because upstream's `Tester::getPort()` bases every instance at the same port
(issue #394) -- so its wall clock is the sum of its tests, and this is the one
number that tells you which ones to look at.

## The virtual clock

Some behaviour can only be observed after real time passes, and the test cannot
shorten the wait. `cron.schedule` is plain five-field crontab syntax, so its
finest resolution is one minute; `cron.expect_within` is only meaningful once the
schedule's *next* occurrence has passed, which is one tick plus a further minute.
Three tests were spending about 175 seconds of the suite's wall clock this way.

A binary configured with `--enable-fpmng-debug-clock` honours
`FPMNG_DEBUG_CLOCK_RATE`, an integer from 1 to 600, and runs **both**
`CLOCK_REALTIME` and `CLOCK_MONOTONIC` -- and the blocking waits derived from
them -- that many times faster. The anchor is taken once in `fpm_init()`, before
anything forks, so the master and every child agree. Issue #396;
`sapi/fpmng/fpm/fpm_debug_clock.h` carries the full rationale, including why a
constant time offset cannot work and why `libfaketime` was rejected.

```sh
./configure --enable-fpmng --enable-fpmng-debug-clock ...
```

Three rules this facility lives by:

- **Off by default and never in a shipped package.** With the flag off the code
  is not in the binary, so there is no variable to set and no way to speed up a
  production master's clock. `build/ci-package-gate.sh` deliberately does not
  pass it.
- **A test opts in, and still passes without it.** The rate goes in the test's
  own `--ENV--` section. Every deadline inside the test stays in *real* seconds
  and stays generous, so a binary built without the flag ignores the variable
  and the test passes at real speed rather than skipping. That is what keeps
  these tests running in the release package gate, which executes the suite on
  Alpine.
- **An assertion may only read a duration the master measured.** A duration the
  *test process* measures with its own `time()` is real, and comparing the two
  would fail. `fpmng-supervisor-max-runtime.phpt` is the worked example: it
  parses the "exited on signal 9 ... after N seconds" figure out of the error
  log, which the master computed on the scaled clock.

At rate 1, or with the variable unset, every reading is the plain libc call and
nothing is logged -- so a debug-clock build behaves identically to one without
the flag, which is what keeps the rest of the suite unaffected. A value that is
not an integer in range logs a `WARNING` and falls back to real speed.

Two clock readings are deliberately **not** scaled: the `clock_gettime()` calls
in `fpm_pool_cron.c` and `fpm_pool_supervisor.c` that exist only as hash entropy
mixed with `getpid()`. They measure nothing.

## Excluding a test from the runner

`sapi/fpmng/tests/not-run-in-ci.list` holds, one per line, a test file name
followed by the reason it cannot run where the runner runs. The runner drops
those from the run and rejects a name with no reason, a name with no file, and
a name without the `fpmng-` prefix — an excluded test keeps the prefix, because
`run-fpm-phpt.sh` reads "not named `fpmng-*`" as "upstream's" and would run it
in the other job. The list is empty today. It is not an escape hatch for a
failing test — that gets fixed or gets an issue.

Its counterpart on the upstream side,
`sapi/fpmng/tests/upstream-deviations.list`, is a different thing: it names
upstream tests that fail because fpm-ng removed the behaviour they test. Our
own tests are never a deviation from upstream, so an `fpmng-` name there is
refused. See [`fpm-phpt.md`](fpm-phpt.md#deliberate-deviations).

## CI

The `fpmng-phpt` job in `.github/workflows/build-matrix.yml` downloads the
canonical build artifact and runs this runner. It is the only job that runs
these tests: before issue #95 the `phpt` job ran them a second time, because
that runner swept the whole copied test directory.
