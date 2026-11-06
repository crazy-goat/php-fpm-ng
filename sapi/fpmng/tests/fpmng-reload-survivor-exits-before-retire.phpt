--TEST--
fpm-ng: a reload survivor that exits on its own before it is retired is reaped, and the master survives (issue #1012)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_no_fault_inject();
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #1012. Pool [s] is a supervisor with three copies. Reload A lowers it to
 * two, so one old copy is spared as a #329 survivor, and the new generation
 * tracks it: fpm_pool_supervisor_reload_survivor_track() queues poll_ev and the
 * exit callback. The fault point survivor_poll_hold keeps the poll from
 * retiring the survivor, so the survivor exits on its own first: its script
 * kills its own process with kill(1) when its pid is in the die file. The
 * master sends no signal, so this is an exit the master did not ask for. A
 * script that returns does not end the process: the copy parks in pause()
 * (fpm_pool_supervisor_park()).
 *
 * The exit callback runs inside fpm_children_bury(), which is a timer callback.
 * It must not delete poll_ev there: fpm_event_loop() saved the queue node after
 * poll_ev before it ran the callback, and a delete frees that node. The master
 * must survive the exit and log "reload survivor pid <S> reaped" (a DEBUG line).
 *
 * The survivor is not retired, so no "retiring reload survivor" line may appear.
 *
 * FPMNG_FAULT is set for the whole run. Only the new generation runs the poll.
 *
 * Not reproduced: the use-after-free needs poll_ev to be the queue node saved
 * after children_bury_timer (docs/timers.md). In this scenario that did not
 * happen (a probe on the old build read 0), so this test passes without the
 * bug. It guards the new behaviour: the exit callback leaves the timer alone. */
$stopTimeout = 2;
$root = sys_get_temp_dir() . '/fpmng-surv-exits-first-' . getmypid();
@mkdir($root, 0700, true);
$alive = "$root/alive.log";
$die = "$root/die";

$cleanup = function () use ($root, $alive, $die) {
    @unlink($die);
    @unlink($alive);
    @unlink("$root/s.php");
    @rmdir($root);
};

/* Writes its pid once, at start. A copy whose pid is in the die file kills
 * itself with SIGKILL, with no signal from the master (the same method as the
 * watchdog test). The other copies run on until the test ends. */
file_put_contents("$root/s.php", <<<PHP
<?php
error_reporting(0);
@file_put_contents('{$alive}', getmypid() . "\\n", FILE_APPEND);
for (;;) {
    if ((int) @file_get_contents('{$die}') === getmypid()) {
        exec('kill -9 ' . getmypid());
    }
    usleep(20000);
}
PHP);

/* log_level = debug: the "reaped" line is a DEBUG message. */
$config = function (int $processes) use ($root, $stopTimeout) {
    return <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = debug
process_control_timeout = 2

[s]
pool.type = supervisor
supervisor.script = $root/s.php
supervisor.processes = $processes
supervisor.restart = always
supervisor.restart_delay = 1
supervisor.stop_timeout = {$stopTimeout}
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

/* True while the pid exists and is not a zombie. */
function pid_alive(int $pid): bool
{
    if ($pid <= 1 || !is_dir("/proc/$pid")) {
        return false;
    }
    $stat = @file_get_contents("/proc/$pid/stat");
    $close = $stat === false ? false : strrpos($stat, ')');
    return $close === false || ($stat[$close + 2] ?? '') !== 'Z';
}

$tester = new FPM\Tester($config(3), '<?php');
$survivor = 0;
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false, false, [], [], array_merge(getenv(), [
        'FPMNG_FAULT' => 'survivor_poll_hold',
    ]));
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $logFile = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
    $master = $tester->getPid();

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
    echo "survivor tracked: ok\n";

    /* The survivor exits now, before any retirement: the poll is held. */
    file_put_contents($die, (string) $survivor);
    check(waitForLog($logFile, "reload survivor pid $survivor reaped", 20),
        "the master did not reap the survivor $survivor after its own exit");
    echo "survivor reaped: ok\n";

    /* Several poll ticks (200 ms each) after the exit, the master must still run. */
    usleep(1500000);
    check(pid_alive($master), 'the master died after the survivor exited on its own');
    echo "master alive: ok\n";

    $log = (string) @file_get_contents($logFile);
    check(substr_count($log, "reload survivor pid $survivor reaped") === 1,
        'the reaped line is not logged exactly once');
    check(!str_contains($log, "retiring reload survivor pid $survivor"),
        'the survivor was retired, although the poll was held');
    echo "reaped once, not retired: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    if ($survivor > 1 && pid_alive($survivor)) {
        exec("kill -KILL $survivor 2>/dev/null");
    }
    usleep(500000);
    $cleanup();
}
?>
--EXPECT--
survivor tracked: ok
survivor reaped: ok
master alive: ok
reaped once, not retired: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
