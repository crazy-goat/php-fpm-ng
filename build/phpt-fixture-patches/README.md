# build/phpt-fixture-patches

Local changes to the upstream test harness that `third_party/php-src/` carries
unedited (`third_party/php-src/README.md`: nothing there is edited by hand, and
`build/vendor-php-src.sh check` refuses an edit). They are applied to the
throwaway test tree by `build/phpt-parallel.sh`, never to the bundle, so the
manifest hashes stay those of the pinned upstream tag and the same patch works
on a tree from `build/prepare.sh`.

| Patch | Issue | What it prevents |
|---|---|---|
| `0001-tester-port-base-per-worker.patch` | #394 | `FPM\Tester::getPort()` starts every Tester at 9008, so two tests in two `run-tests.php -j` workers bind the same port. The base now moves by 200 per `TEST_PHP_WORKER`; unset or 0 keeps 9008, 9009, ... |
| `0002-run-tests-worker-env-for-tests.patch` | #394 | `run-tests.php` sets `TEST_PHP_WORKER` only in the worker process; a test is started with the main process's environment and never saw it. The patch adds it to the environment the worker hands its tests. |
| `0003-run-tests-no-retry-on-port-collision.patch` | #562 | `run-tests.php` retries a failed test once when its output says "address already in use" (glibc; musl says "Address in use" and never matched) (and for any test that calls usleep/sleep/microtime/hrtime). A port collision under `-j` therefore showed as "WARN passed on retry", which `build/ci-package-gate.sh` counts as a pass (#301). The collision retry is gone, the timing retry stays: the gate has recorded `fpmng-http-direct-worker-saturation-refuses-new.phpt` passing only on retry on a loaded runner. |

The patches are written against the layout of the assembled tree (`a/sapi/fpmng/tests/tester.inc`, `a/run-tests.php`) and applied with `patch -p1`.

The patches are made against `php-8.5.9`. A new pin that changes the patched
lines makes `build/phpt-parallel.sh` fail loudly; rebase the patch then.
