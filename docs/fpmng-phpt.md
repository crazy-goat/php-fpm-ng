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
Five tests were spending well over a hundred seconds of the suite's wall clock
this way.

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
- **Only the master's C code is scaled, so an assertion may only read a time
  the master measured.** This is the rule that decides whether a given test can
  be converted at all, and it excludes more tests than it admits.
  `fpmng-supervisor-max-runtime.phpt` is the worked example of the good case: it
  parses the "exited on signal 9 ... after N seconds" figure out of the error
  log, which the master computed on the scaled clock.

  PHP's own `time()`, `gmdate()` and `microtime()` are **not** scaled -- neither
  in the test process nor in a cron job script or a supervisor iteration script,
  which are ordinary PHP processes. So a test that compares a timestamp one of
  those recorded against a configured interval cannot be accelerated: at rate 10
  a 20-virtual-second jitter bound is two real seconds, while the job script
  writes a real second-of-minute, and the comparison becomes meaningless.

  Measured on run 35274092140, these are the suite's remaining slow tests and
  why each one is or is not converted:

  | test | s | converted | why |
  |---|---|---|---|
  | `fpmng-cron-jitter` | 67.1 | no | the job script records `gmdate('s')` and the test checks it against `cron.jitter`: a real reading against a virtual bound |
  | `fpmng-cron-schedule` | 40.0 | **yes** | asserts only that the marker exists; the `gmdate('c')` it writes is never compared |
  | `fpmng-baseline-counters-cron` | 2.0 / 40.1 | **yes** | asserts only on master-produced figures (the `fpmng_pool_runs_total` series and the status page) |
  | `fpmng-supervisor-jitter` | 25.1 | no | same as cron-jitter, on `microtime(true)` deltas written by the iteration script |
  | `fpmng-supervisor-max-memory` | 14.7 | no | almost all of it is a 15 s negative-proof budget in the *test* process; the recycle itself is instant |
  | `fpmng-supervisor-reload-rolling` | 12.5 | no | 10 of it is `supervisor.stop_timeout`, spent in the watchdog, which counts real seconds by design |
  | `fpmng-cron-expect-within` | 12.3 | **yes** | asserts on master log patterns only |
  | `fpmng-cron-stop-signal` | 6.0 | **yes** | asserts on master log patterns only |
  | `fpmng-supervisor-heartbeat` | 5.9 | no | its `heartbeat_age` threshold is calibrated against a script whose own `usleep()` is real |

  The two figures for `fpmng-baseline-counters-cron` are the same passing test in
  the canonical and the fiber suite of the same run. Nothing differs but where in
  the minute it started: waiting for a `* * * * *` tick is a 0-60 s lottery, so
  these tests add variance to the suite's duration and not just time. At rate 10
  the lottery is 0-6 s.

  Two of those "no" rows are cheap to fix without the clock at all, and the
  reasons above are why. `fpmng-supervisor-reload-rolling` pays
  `supervisor.stop_timeout` because the survivor the reload spares runs an
  unconditional `for (;;)`: it never returns to the signal handler that the
  retiring kill set a flag in, so the pool's watchdog ends it with SIGKILL ten
  seconds later, and `fpm_pool_watchdog.c` counts those seconds with a plain
  `sleep(1)` on purpose -- a watchdog that a test environment variable could
  speed up would not be a watchdog. Setting `supervisor.stop_timeout = 1` in
  the test's own config recovers most of it. `fpmng-supervisor-max-memory`
  spends its budget proving `restart = never` does not restart; the master says
  so positively in the error log ("restart = never -> not restarting"), which
  the test already reads, so waiting for that line and keeping a short negative
  window afterwards recovers most of it. Both are test design, tracked
  separately from this facility.

  Accelerating any of the four "no" rows by *clock* needs the assertion rewritten to read a
  master-measured figure first -- the way `fpmng-cron-jitter` already cross-checks
  the operator status page against the child's recorded offset. That is a
  separate piece of work, not a rate in an `--ENV--` section.

At rate 1, or with the variable unset, every reading is the plain libc call and
nothing is logged -- so a debug-clock build behaves identically to one without
the flag, which is what keeps the rest of the suite unaffected. A value that is
not an integer in range logs a `WARNING` and falls back to real speed.

Two clock readings are deliberately **not** scaled: the `clock_gettime()` calls
in `fpm_pool_cron.c` and `fpm_pool_supervisor.c` that exist only as hash entropy
mixed with `getpid()`. They measure nothing.

A stamp and the reading it is subtracted from must come from the **same** clock,
and the operator page is where that is easy to get wrong. `uptime`,
`backoff_seconds` and `heartbeat_age` are not stored; each renderer in
`fpm_operator_pages.c` derives them at render time from `last_start`,
`backoff_until` and `last_heartbeat`, all three of which the pool code writes
with `FPM_NOW()`. So the renderers read `FPM_NOW()` too. They used to read
`time(NULL)`, which was correct only at rate 1: under any higher rate the
virtual clock runs ahead of the real one, so `uptime` and `heartbeat_age` would
have clamped to 0 and `backoff_seconds` would have been inflated by the whole
drift -- a silently wrong number on a page a test asserts against, not a
failure. None of the five converted tests read those three fields, so nothing
was reporting wrongly; it was found by audit, not by a red test.

The gateway and the HTTP worker are the other half of the rule, and they are
consistent the other way: `fpm_http_direct_worker.c` writes `fw.start_time` with
`time(NULL)` and compares it against `time(NULL)`, and the access logs format
wall-clock time, which must stay real. That path is not scaled at all -- both
sides of every comparison in it are real -- so a test of it cannot be
accelerated, but neither can it be made inconsistent.

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
