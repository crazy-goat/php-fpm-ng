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

/* Two independent halves, and the first of them used to be missing (issue
 * #399). The master states the decision outright -- fpm_reload_selective.c
 * logs "[pool keep] issue #330: config unchanged -- sparing all N running
 * child(ren)" at NOTICE in the same function that puts the pids into
 * FPM_RELOAD_SELECTIVE_ENV for the next generation to adopt -- so that line is
 * positive proof, available as soon as the old master reaches execvp(). It is
 * read out of the error log directly rather than through the log tool, because
 * expectLogReloadingNotices() above has already walked past it.
 *
 * The pid window after it is still necessary, and is what this test was built
 * on: the NOTICE says the old master meant to spare the child, the window says
 * the child really is the same process afterwards, including that it never
 * briefly flipped and came back. But it no longer has to carry the whole proof
 * on its own, so one second of it is enough where three were budgeted. */
$logFile = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
$keepSpared = false;
$spareDeadline = time() + 15;
while (time() < $spareDeadline) {
    if (preg_match('/\[pool keep\][^\n]*config unchanged -- sparing all 1 running/',
            (string) @file_get_contents($logFile))) {
        $keepSpared = true;
        break;
    }
    usleep(100000);
}

$keepPidsSeen = [$keepPidBefore => true];
$pollDeadline = microtime(true) + 1.0;
while (microtime(true) < $pollDeadline) {
    $data = @file_get_contents($keepPidFile);
    if ($data !== false && $data !== '' && preg_match('/^[0-9]+$/', $data)) {
        $keepPidsSeen[(int) $data] = true;
    }
    usleep(50000);
}

if ($changePidAfter === $changePidBefore) {
    echo "FAIL: [change] pool did not restart across the reload (pid stayed $changePidBefore)\n";
} elseif (!$keepSpared) {
    echo "FAIL: the master never logged that it was sparing [keep]'s child; "
        . "reload.selective = yes did not take effect on an unchanged pool\n";
} elseif (count($keepPidsSeen) > 1) {
    printf("FAIL: [keep] pool's worker pid changed across the reload (saw: %s) -- "
        . "reload.selective = yes must leave an unchanged pool untouched\n",
        implode(', ', array_keys($keepPidsSeen)));
} else {
    echo "reload-selective-on: ok\n";
}

if ($changePidAfter === $changePidBefore || !$keepSpared || count($keepPidsSeen) > 1) {
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
