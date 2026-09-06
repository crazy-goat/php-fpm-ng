# Upstream FPM PHPT tests

`build/prepare.sh` copies upstream `sapi/fpm/` to `sapi/fpmng/` and then overlays
only the files owned by this repository. The copied `sapi/fpmng/tests/` directory
is deliberately retained. The tests remain upstream files; do not copy them into
this repository or edit their expected output.

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
  `pool.type`, `php-fpm-ng` or `fastcgi-ng` marker.

A binary that does not pass this fingerprint check produces a `NOT MEASURED`
result instead of a measurement.

## Result files

The result directory contains:

- `discovered.tsv`: sorted list of copied upstream `.phpt` files;
- `statuses.raw.tsv`: statuses emitted by PHP's `run-tests.php`;
- `results.tsv`: one row per discovered test with `test`, `category` and
  `raw_status` columns;
- `summary.txt`: category counts and the runner exit status;
- `metadata.txt`: source commit, executable paths, versions, hashes, strings
  markers and test environment;
- `run.log`, `test-output.log` and `failed.raw.txt`: upstream harness output.

The categories are intentionally stricter than PHP's result names:

| Runner category | Upstream statuses |
|---|---|
| `PASS` | `PASSED` only |
| `SKIP` | `SKIPPED` only |
| `FAIL/ERROR` | `FAILED`, `BORKED`, `WARNED`, `LEAKED`, `XFAILED`, `XLEAKED` or any other emitted non-pass status |
| `NOT MEASURED` | no status was emitted, or a prerequisite prevented the run |

A missing status is never converted to `PASS`. A non-zero runner exit status is
reported separately and does not turn a measured failure into a skip or a pass.

## Triage policy

This task does not weaken, delete or fork upstream tests. Every future
`FAIL/ERROR` must be recorded as exactly one of:

1. **our bug** — `php-fpm-ng` differs from correct upstream behavior, with a
   separate task file;
2. **intended difference** — the difference is deliberate and points to the
   document that specifies it; or
3. **test artifact** — the test assumes an upstream-only path, binary name or
   build detail, with that assumption recorded.

A `NOT MEASURED` row is not a failure verdict and must not be used to claim a
pass rate. The current committed inventory and blocker are in
[`fpm-phpt-results.md`](fpm-phpt-results.md).
