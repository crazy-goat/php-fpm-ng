--TEST--
fpm-ng: reload.selective = no (default) still restarts every pool on a reload (issue #330)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Baseline for issue #330: with the feature off (the default -- this config
 * does not even set reload.selective, so fpm_global_config.reload_selective
 * stays its zero-initialized default), a reload must behave exactly like it
 * always has -- EVERY pool restarts, even one whose own section did not
 * change at all between the two configs below. This is the regression this
 * whole feature must never cause. */
$work = sys_get_temp_dir() . '/fpmng-reload-sel-off-' . getmypid();
@mkdir($work, 0700, true);
$unrelatedPidFile = "$work/unrelated.pid";
$changedPidFile = "$work/changed.pid";
@unlink($unrelatedPidFile);
@unlink($changedPidFile);

$cleanup = function () use ($work, $unrelatedPidFile, $changedPidFile) {
    @unlink($unrelatedPidFile);
    @unlink($changedPidFile);
    @unlink("$work/write-pid.php");
    @rmdir($work);
};

/* Overwrites (not appends): only the CURRENT pid matters for this test --
 * unlike fpmng-supervisor-reload-rolling.phpt this is not measuring gap size,
 * only "is it still the same pid after the reload". */
$script = <<<PHP
<?php
error_reporting(0);
file_put_contents(getenv('FPMNG_PIDFILE'), getmypid());
for (;;) {
    file_put_contents(getenv('FPMNG_PIDFILE'), getmypid());
    usleep(50000);
}
PHP;
file_put_contents("$work/write-pid.php", $script);

$cfgBefore = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
[unrelated]
pool.type = supervisor
supervisor.script = $work/write-pid.php
supervisor.processes = 1
supervisor.restart = always
env[FPMNG_PIDFILE] = $unrelatedPidFile
[changed]
pool.type = supervisor
supervisor.script = $work/write-pid.php
supervisor.processes = 1
supervisor.restart = always
supervisor.restart_delay = 1
env[FPMNG_PIDFILE] = $changedPidFile
EOT;

/* Only [changed] differs between the two configs -- [unrelated] is
 * byte-for-byte identical in both, which is exactly what would let
 * reload.selective = yes spare it. Here it must NOT be spared: the setting
 * is off. */
$cfgAfter = str_replace('supervisor.restart_delay = 1', 'supervisor.restart_delay = 2', $cfgBefore);

$tester = new FPM\Tester($cfgBefore, $script);
$tester->start();
$tester->expectLogStartNotices();

$waitForPid = function (string $file, int $timeoutSeconds = 15): int {
    $deadline = time() + $timeoutSeconds;
    while (time() < $deadline) {
        $data = @file_get_contents($file);
        if ($data !== false && $data !== '' && preg_match('/^[0-9]+$/', $data)) {
            return (int) $data;
        }
        usleep(100000);
    }
    throw new RuntimeException("no pid ever written to $file");
};

$unrelatedPidBefore = $waitForPid($unrelatedPidFile);
$changedPidBefore = $waitForPid($changedPidFile);

$tester->reload($cfgAfter);
$tester->expectLogReloadingNotices(0);

/* Wait for [changed]'s pid file to show a pid different from before -- proof
 * the reload actually completed and this pool restarted (it always does,
 * changed or not, with the feature off). */
$deadline = time() + 20;
$changedPidAfter = $changedPidBefore;
while (time() < $deadline && $changedPidAfter === $changedPidBefore) {
    $data = @file_get_contents($changedPidFile);
    if ($data !== false && $data !== '' && preg_match('/^[0-9]+$/', $data)) {
        $changedPidAfter = (int) $data;
    }
    usleep(100000);
}

/* Give [unrelated] the same amount of settling time before reading its
 * final pid, so a slow-but-real restart is not mistaken for "never
 * restarted" just because this check ran first. */
usleep(500000);
$unrelatedPidAfter = (int) @file_get_contents($unrelatedPidFile);

if ($changedPidAfter === $changedPidBefore) {
    echo "FAIL: [changed] pool did not restart across the reload (pid stayed $changedPidBefore)\n";
} elseif ($unrelatedPidAfter === $unrelatedPidBefore) {
    echo "FAIL: [unrelated] pool did not restart across the reload (pid stayed $unrelatedPidBefore) "
        . "-- reload.selective = no must restart every pool, unchanged or not\n";
} else {
    echo "reload-selective-off: ok\n";
}

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

$cleanup();

?>
Done
--EXPECT--
reload-selective-off: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
