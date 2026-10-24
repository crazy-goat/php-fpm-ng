--TEST--
fpm-ng: a reload survivor whose watchdog cannot be forked is SIGKILLed by its master (issue #890)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_no_fault_inject();
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #890. Pool [s] is a supervisor with three copies. Reload A lowers it to
 * two, so one old copy is spared as a #329 survivor. The new generation tracks
 * it and retires it when its first copy has started: the retirement sends the
 * stop signal. The survivor's own SIGTERM handler forks its watchdog, and the
 * fault point pool_watchdog_fork makes that fork() fail. The survivor's script
 * does not end on the stop signal, so nothing else ends it.
 *
 * The master backstop (fpm_pool_supervisor_reload_survivor_backstop()) must
 * SIGKILL the survivor stop_timeout + 1 s after the stop signal, and log one
 * WARNING that says so. The test checks that WARNING, and that the pid is gone.
 *
 * FPMNG_FAULT is set for the whole run, not for one master. The survivor was
 * forked by the first master, so its environment has no
 * FPMNG_RELOAD_SURVIVORS, and an ifenv= condition would not reach it. The point
 * only fails a watchdog fork, and this test starts no other watchdog before the
 * stop signal.
 *
 * Mutation: delete the fpm_event_add() of the backstop in
 * fpm_pool_supervisor_reload_survivor_retire(). The survivor then lives on, and
 * the WARNING is never logged. */
$root = sys_get_temp_dir() . '/fpmng-surv-wdfork-' . getmypid();
@mkdir($root, 0700, true);
$alive = "$root/alive.log";
$stop = "$root/stop";

$cleanup = function () use ($root, $alive, $stop) {
    @unlink($stop);
    @unlink($alive);
    @unlink("$root/s.php");
    @rmdir($root);
};

/* Writes its pid once, at start. The stop file ends the copy when the test is
 * over: the copy checks it every 20 ms and kills itself with kill(1). exit()
 * is not enough, a supervisor copy does not end with its script. */
file_put_contents("$root/s.php", <<<PHP
<?php
error_reporting(0);
@file_put_contents('{$alive}', getmypid() . "\\n", FILE_APPEND);
for (;;) {
    if (file_exists('{$stop}')) {
        exec('kill -9 ' . getmypid());
    }
    usleep(20000);
}
PHP);

/* supervisor.stop_timeout = 1 is the minimum: the backstop fires 2 s after the
 * stop signal, and the test waits for that. process_control_timeout = 2 lets
 * the master SIGKILL the copies at shutdown, because their watchdogs fail too. */
$config = function (int $processes) use ($root) {
    return <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
process_control_timeout = 2

[s]
pool.type = supervisor
supervisor.script = $root/s.php
supervisor.processes = $processes
supervisor.restart = always
supervisor.restart_delay = 1
supervisor.stop_timeout = 1
EOT;
};

function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function waitForLog(string $file, string $needle, int $seconds): bool
{
    $deadline = time() + $seconds;
    while (time() < $deadline) {
        if (str_contains((string) @file_get_contents($file), $needle)) {
            return true;
        }
        usleep(100000);
    }
    return false;
}

/* A zombie counts as gone: the survivor is a child of the test's master, and
 * only the master reaps it. */
function pid_alive(int $pid): bool
{
    if ($pid <= 1 || !is_dir("/proc/$pid")) {
        return false;
    }
    $stat = @file_get_contents("/proc/$pid/stat");
    $close = $stat === false ? false : strrpos($stat, ')');
    return $close === false || ($stat[$close + 2] ?? '') !== 'Z';
}

function wait_gone(int $pid, int $seconds): bool
{
    $deadline = microtime(true) + $seconds;
    do {
        if (!pid_alive($pid)) {
            return true;
        }
        usleep(50000);
    } while (microtime(true) < $deadline);
    return false;
}

$tester = new FPM\Tester($config(3), '<?php');
$survivor = 0;
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false, false, [], [], array_merge(getenv(), [
        'FPMNG_FAULT' => 'pool_watchdog_fork',
    ]));
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $logFile = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);

    $deadline = time() + 15;
    $pids = [];
    while (time() < $deadline && count($pids) < 3) {
        foreach (explode("\n", (string) @file_get_contents($alive)) as $line) {
            if (trim($line) !== '') {
                $pids[trim($line)] = true;
            }
        }
        usleep(100000);
    }
    check(count($pids) >= 3, 'before the reload: pool [s] did not start three copies');

    $tester->reload($config(2));
    check(waitForLog($logFile, 'is still running the previous generation', 20),
        'the reload did not spare and track a survivor');
    $log = (string) @file_get_contents($logFile);
    check(preg_match('/reload survivor pid (\d+) is still running the previous/', $log, $m) === 1,
        'the log does not name the survivor');
    $survivor = (int) $m[1];

    check(waitForLog($logFile, "retiring reload survivor pid $survivor (replacement confirmed running)", 20),
        'the survivor was not retired when its replacement started');
    echo "survivor retired: ok\n";

    /* The survivor's watchdog never started, so only the backstop can end it. */
    check(waitForLog($logFile, "reload survivor pid $survivor is still running 2 s after its stop signal; "
        . "its watchdog did not end it, so sending SIGKILL (issue #890)", 20),
        'the master did not SIGKILL the survivor after stop_timeout + 1 s');
    check(wait_gone($survivor, 10), "the survivor $survivor is still alive after the master's SIGKILL");
    echo "survivor SIGKILLed by the master: ok\n";

    $log = (string) @file_get_contents($logFile);
    check(substr_count($log, "reload survivor pid $survivor is still running 2 s") === 1,
        'the backstop WARNING is not logged exactly once');
    echo "warning logged once: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    /* By pid, and only this survivor: the copies end on the stop file. */
    if ($survivor > 1 && pid_alive($survivor)) {
        exec("kill -KILL $survivor 2>/dev/null");
    }
    @touch($stop);
    usleep(500000);
    $cleanup();
}
?>
--EXPECT--
survivor retired: ok
survivor SIGKILLed by the master: ok
warning logged once: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
