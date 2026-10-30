--TEST--
fpm-ng: a reload survivor of a pool that the new configuration removes gets SIGTERM at its deadline (issue #889)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('supervisor');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #889. Pool [s] is a supervisor with two copies. Reload A removes [s]
 * and adds pool [p], so the new generation runs no supervisor. The old master
 * spares one copy of [s] as a #329 survivor. Before issue #889 no master
 * tracked it: the new generation has no init_main() for [s], so nothing sent
 * the survivor a stop signal, and it ran until its own script ended. Its script
 * does not end on a signal, so only a stop signal from the master can end it.
 *
 * The new generation must log the survivor, keep it running until its deadline
 * (about 30 s after reload A), send it SIGTERM at the deadline, and log that.
 * The survivor's own watchdog then ends it after supervisor.stop_timeout.
 *
 * The test uses the real clock. The log stamps have whole seconds, so the gap
 * between the two log lines is checked with a margin of one second.
 *
 * Mutation: delete the call of fpm_pool_supervisor_reload_orphans_arm() in
 * fpm_run() (fpm.c). Then the new generation never logs the survivor, and the
 * first wait fails. */
$stopTimeout = 2;
$root = sys_get_temp_dir() . '/fpmng-surv-removed-' . getmypid();
@mkdir($root, 0700, true);
$alive = "$root/alive.log";
$stop = "$root/stop";

$cleanup = function () use ($root, $alive, $stop) {
    @unlink($stop);
    @unlink($alive);
    @unlink("$root/s.php");
    @unlink("$root/p.php");
    @rmdir($root);
};

/* Writes its pid once, at start. The copy checks the stop file every 20 ms and
 * kills itself with kill(1) when the file exists. This is the end of the test:
 * a supervisor copy does not end with its script, and a SIGTERM does not end the
 * script either. */
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
file_put_contents("$root/p.php", "<?php\n");

/* Reload A: pool [s] with two copies. */
$configA = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
process_control_timeout = 2

[s]
pool.type = supervisor
supervisor.script = $root/s.php
supervisor.processes = 2
supervisor.restart = always
supervisor.restart_delay = 1
supervisor.stop_timeout = {$stopTimeout}
EOT;

/* Reload B: pool [s] is gone. Pool [p] is an http-direct pool, so the new
 * generation has no supervisor pool and no init_main() for [s]. Its front
 * controller must be a regular file in chdir, or the reload is refused. */
$configB = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
process_control_timeout = 2

[p]
listen = {{ADDR[p]}}
chdir = $root
pm = static
pm.max_children = 1
pool.type = http-direct
http.front_controller = /p.php
EOT;

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

/* At notice level zlog prints "[dd-Mon-yyyy hh:mm:ss] LEVEL: message", with
 * whole seconds. Returns the stamp of the first line that contains $needle. */
function logTime(string $log, string $needle): int
{
    $pattern = '/^\[(\d\d-[A-Za-z]{3}-\d{4} \d\d:\d\d:\d\d)\] [A-Z]+: [^\n]*'
        . preg_quote($needle, '/') . '/m';
    check(preg_match($pattern, $log, $m) === 1, "the log has no line with: $needle");
    return DateTimeImmutable::createFromFormat('d-M-Y H:i:s', $m[1])->getTimestamp();
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

$tester = new FPM\Tester($configA, '<?php');
$survivor = 0;
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false, false, [], []);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $logFile = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);

    $deadline = time() + 15;
    $pids = [];
    while (time() < $deadline && count($pids) < 2) {
        foreach (explode("\n", (string) @file_get_contents($alive)) as $line) {
            if (trim($line) !== '') {
                $pids[trim($line)] = true;
            }
        }
        usleep(100000);
    }
    check(count($pids) >= 2, 'before the reload: pool [s] did not start two copies');

    $tester->reload($configB);
    check(waitForLog($logFile, 'belongs to no pool of this configuration', 20),
        'the new generation did not track the survivor of the removed pool');
    $log = (string) @file_get_contents($logFile);
    check(preg_match('/reload survivor pid (\d+) belongs to no pool of this configuration/', $log, $m) === 1,
        'the log does not name the survivor');
    $survivor = (int) $m[1];
    check(pid_alive($survivor), "the survivor $survivor is not running after the reload");
    echo "survivor tracked: ok\n";

    /* Before the deadline the survivor is not signalled. */
    sleep(5);
    check(pid_alive($survivor), "the survivor $survivor ended before its deadline");
    check(!str_contains((string) @file_get_contents($logFile), 'sending SIGTERM (issue #889)'),
        'SIGTERM was sent before the deadline');
    echo "survivor kept before its deadline: ok\n";

    check(waitForLog($logFile, "reload survivor pid $survivor is still running at its deadline", 45),
        'the survivor did not get SIGTERM at its deadline');
    echo "survivor signalled at its deadline: ok\n";

    /* The stamps have whole seconds. The deadline is the reload's exec + 30 s,
     * and the poll fires within 200 ms after it. The arm line is logged within
     * a second of the exec. So the gap is 29 to 31 s. */
    $log = (string) @file_get_contents($logFile);
    $armedAt = logTime($log, "reload survivor pid $survivor belongs to no pool of this configuration");
    $signalledAt = logTime($log, "reload survivor pid $survivor is still running at its deadline");
    $gap = $signalledAt - $armedAt;
    check($gap >= 29 && $gap <= 31, "SIGTERM came $gap s after the survivor was tracked, not about 30 s");
    echo "deadline timing: ok\n";

    /* SIGTERM starts the survivor's own watchdog, which ends the copy after
     * supervisor.stop_timeout. */
    check(wait_gone($survivor, 15), "the survivor $survivor is still alive after SIGTERM and its watchdog");
    echo "survivor ended after SIGTERM: ok\n";

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
survivor tracked: ok
survivor kept before its deadline: ok
survivor signalled at its deadline: ok
deadline timing: ok
survivor ended after SIGTERM: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
