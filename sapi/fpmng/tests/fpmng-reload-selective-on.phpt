--TEST--
fpm-ng: reload.selective = yes leaves an unchanged pool's worker untouched across a reload (issue #330)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* The core value of issue #330: with reload.selective = yes, a pool whose
 * config section did not change between the old and new config files is not
 * restarted at all -- same worker pid before and after. A pool whose section
 * DID change is still restarted, exactly like today. */
$work = sys_get_temp_dir() . '/fpmng-reload-sel-on-' . getmypid();
@mkdir($work, 0700, true);
$keepPidFile = "$work/keep.pid";
$changePidFile = "$work/change.pid";
@unlink($keepPidFile);
@unlink($changePidFile);

$cleanup = function () use ($work, $keepPidFile, $changePidFile) {
    @unlink($keepPidFile);
    @unlink($changePidFile);
    @unlink("$work/write-pid.php");
    @rmdir($work);
};

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
reload.selective = yes
[keep]
pool.type = supervisor
supervisor.script = $work/write-pid.php
supervisor.processes = 1
supervisor.restart = always
env[FPMNG_PIDFILE] = $keepPidFile
[change]
pool.type = supervisor
supervisor.script = $work/write-pid.php
supervisor.processes = 1
supervisor.restart = always
supervisor.restart_delay = 1
env[FPMNG_PIDFILE] = $changePidFile
EOT;

/* [keep] is byte-for-byte identical in both configs; [change] is not. */
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

$keepPidBefore = $waitForPid($keepPidFile);
$changePidBefore = $waitForPid($changePidFile);

$tester->reload($cfgAfter);
$tester->expectLogReloadingNotices(0);

/* Wait for [change]'s pid to actually flip -- proof the reload completed. */
$deadline = time() + 20;
$changePidAfter = $changePidBefore;
while (time() < $deadline && $changePidAfter === $changePidBefore) {
    $data = @file_get_contents($changePidFile);
    if ($data !== false && $data !== '' && preg_match('/^[0-9]+$/', $data)) {
        $changePidAfter = (int) $data;
    }
    usleep(100000);
}

/* [keep]'s pid file is being overwritten every 50ms by the SAME process this
 * whole time if it was never restarted -- read it well after the reload has
 * settled, and also confirm the reader never observed a different pid along
 * the way (a brief flip-then-back would still be a bug: it would mean the
 * pool restarted, its config not actually being what this test believes it
 * is). */
$keepPidsSeen = [$keepPidBefore => true];
$pollDeadline = time() + 3;
while (time() < $pollDeadline) {
    $data = @file_get_contents($keepPidFile);
    if ($data !== false && $data !== '' && preg_match('/^[0-9]+$/', $data)) {
        $keepPidsSeen[(int) $data] = true;
    }
    usleep(50000);
}

if ($changePidAfter === $changePidBefore) {
    echo "FAIL: [change] pool did not restart across the reload (pid stayed $changePidBefore)\n";
} elseif (count($keepPidsSeen) > 1) {
    printf("FAIL: [keep] pool's worker pid changed across the reload (saw: %s) -- "
        . "reload.selective = yes must leave an unchanged pool untouched\n",
        implode(', ', array_keys($keepPidsSeen)));
} else {
    echo "reload-selective-on: ok\n";
}

if ($changePidAfter === $changePidBefore || count($keepPidsSeen) > 1) {
    $tester->close(true);
    $cleanup();
    exit(1);
}

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

$cleanup();

?>
Done
--EXPECT--
reload-selective-on: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
