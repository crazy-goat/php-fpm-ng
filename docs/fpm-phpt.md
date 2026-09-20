# Upstream FPM PHPT tests

`build/prepare.sh` copies upstream `sapi/fpm/` to `sapi/fpmng/` and then overlays
only the files owned by this repository. The copied `sapi/fpmng/tests/` directory
is deliberately retained. The tests remain upstream files; do not copy them into
this repository or edit their expected output.

`run-fpm-phpt.sh` runs the upstream tests only. Our own `fpmng-*.phpt` land in
the same directory and are excluded by name; they are run by
[`run-fpmng-phpt.sh`](fpmng-phpt.md).

A few upstream tests cover behaviour `php-fpm-ng` removed on purpose, so they
can only fail. They are named in `sapi/fpmng/tests/upstream-deviations.list`;
see [Deliberate deviations](#deliberate-deviations).

## Prerequisites

- A PHP source checkout with `sapi/fpm/` and `run-tests.php`.
- A source revision compatible with the patch stack in `patches/`. `prepare.sh`
  stops instead of silently applying an incompatible or already-applied patch.
- A completed `php-fpm-ng` build from the prepared source tree and a CLI PHP
  executable from that same PHP build. Supply both as absolute paths.
- `strings` and either `shasum -a 256` or `sha256sum`.
- A dedicated results directory. Tests create sockets, configuration files and
  logs below the copied test directory and may need root, users, IPv4/IPv6 and
  local networking. Set `TEST_FPM_RUN_AS_ROOT=1` only when intentionally running
  as root. Set `TEST_FPM_EXTENSION_DIR` when the test build needs shared
  extensions.

The runner defaults each test timeout to 60 seconds. Override it with the
positive integer `TEST_FPM_TIMEOUT`; this is a per-test limit, not a replacement
for observing the complete command.

## Prepare and run

Set the repository path, prepare a PHP source checkout and rebuild after the
script reports a changed source list:

```sh
REPO=/path/to/php-fpm-ng
"$REPO/build/prepare.sh" /path/to/php-src
cd /path/to/php-src
./buildconf --force
cd /path/to/php-build
./config.nice
make
```

Run the complete copied suite with one command. The two environment variables
are required and are not inferred from `PATH`:

```sh
TEST_PHP_EXECUTABLE=/path/to/php-build/sapi/cli/php \
TEST_PHP_FPM_EXECUTABLE=/path/to/php-build/sapi/fpmng/php-fpm-ng \
TEST_FPM_TIMEOUT=60 \
"$REPO/build/run-fpm-phpt.sh" \
    /path/to/php-src \
    /path/to/dedicated/fpm-phpt-results
```

The FPM harness inherited from upstream looks for `fpm/php-fpm` relative to
its executable. `run-fpm-phpt.sh` creates temporary compatibility symlinks
inside the results directory and removes them on exit. It never changes the
external build directory and records both the requested binary and the
compatibility path in `metadata.txt`.

Before any test is started, the runner records and verifies:

- resolved path and `-v` output for the CLI and FPM binaries;
- SHA-256 for both binaries; and
- distinctive `strings` output containing an `fpmng_` marker and a
  `pool.type`, `php-fpm-ng` or `fpmng_` marker.

A binary that does not pass this fingerprint check produces a `NOT MEASURED`
result instead of a measurement.

## Result files

The result directory contains:

- `discovered.tsv`: sorted list of copied upstream `.phpt` files;
- `statuses.raw.tsv`: statuses emitted by PHP's `run-tests.php`;
- `results.tsv`: one row per discovered test with `test`, `category` and
  `raw_status` columns;
- `deviations.tsv`: the parsed `upstream-deviations.list`, one `test` and
  `reason` per row (empty when nothing is declared);
- `summary.txt`: category counts, the declared deviation count and the runner
  exit status;
- `metadata.txt`: source commit, executable paths, versions, hashes, strings
  markers and test environment;
- `run.log`, `test-output.log` and `failed.raw.txt`: upstream harness output.

The categories are intentionally stricter than PHP's result names:

| Runner category | Upstream statuses |
|---|---|
| `PASS` | `PASSED` only |
| `SKIP` | `SKIPPED` only |
| `WARN` | `WARNED` (for example, an `XFAIL` test passed unexpectedly) |
| `FAIL/ERROR` | `FAILED`, `BORKED`, `LEAKED`, `XFAILED`, `XLEAKED` or any other emitted non-pass status |
| `DEVIATION` | a failure by a test named in `upstream-deviations.list` |
| `UNEXPECTED <category>` | any other result by a test named in `upstream-deviations.list` |
| `NOT MEASURED` | no status was emitted, or a prerequisite prevented the run |

A missing status is never converted to `PASS`. The runner exits non-zero on any
`FAIL/ERROR`, any `NOT MEASURED` and any `UNEXPECTED`; a measured failure is
never turned into a skip or a pass.

## Deliberate deviations

`sapi/fpmng/tests/upstream-deviations.list` names, one per line with a mandatory
reason, the upstream tests that cover behaviour this project removed on purpose:

```
status-listen.phpt	issue #278 removed operator.status_listen and the shared status pool; ...
```

This is triage category 2 below — *intended difference* — written down where the
runner can check it, rather than in prose nobody re-reads. It is not an xfail
bucket and not a place to park a flake: a test that fails for any other reason
is a bug to fix or an issue to open, exactly as before.

The listed tests still run. The runner refuses to start on an entry without a
reason, on a name it cannot find among the discovered tests, and on an
`fpmng-`prefixed name (those are ours, and belong in
[`not-run-in-ci.list`](fpmng-phpt.md)). If a listed test stops failing, the run
fails with `UNEXPECTED`: either the behaviour came back, or the entry outlived
the reason and must go.

The upstream test files themselves are never edited — the whole point of keeping
the copy is that it is upstream's, unchanged.

## Triage policy

This task does not weaken, delete or fork upstream tests. Every future
`FAIL/ERROR` must be recorded as exactly one of:

1. **our bug** — `php-fpm-ng` differs from correct upstream behavior, with a
   separate task file;
2. **intended difference** — the difference is deliberate and points to the
   document that specifies it; record it in
   [`upstream-deviations.list`](#deliberate-deviations); or
3. **test artifact** — the test assumes an upstream-only path, binary name or
   build detail, with that assumption recorded.

A `NOT MEASURED` row is not a failure verdict and must not be used to claim a
pass rate. The current committed inventory and blocker are in
[`fpm-phpt-results.md`](fpm-phpt-results.md).
