--TEST--
fpm-ng: a reload survivor pid that is not a child of the master is not tracked or signalled (issue #891)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('supervisor');
if (!is_readable('/proc/self/stat')) {
    die('skip the start time that identifies a carried pid is read from /proc (Linux only)');
}
?>
--FILE--
<?php

require_once "tester.inc";

/* The master of a reload reads the pid of a spared supervisor child from
 * FPMNG_RELOAD_SURVIVORS. That pid can be stale: the child was reaped before
 * the exec and the kernel gave the pid to an unrelated process. Here the
 * unrelated process is a PHP process that the test starts itself, so it is
 * not a child of the master. The env var names its pid as a survivor of pool
 * [s], with a deadline in the future.
 *
 * Before issue #891 the master tracked it (kill(pid, 0) succeeded) and sent it
 * the stop signal when the replacement copy of [s] started. The unrelated
 * process then died. The fix refuses the pid, and the process keeps running.
 * The test checks its heartbeat: it must keep growing after the master has
 * refused the pid.
 *
 * The env entry carries the start time of the process the pid had when it was
 * spared. Here that start time is one tick off, so the master must refuse the
 * entry on the start time, as it would for a recycled pid. */
$root = sys_get_temp_dir() . '/fpmng-survivor-pid-not-child-' . getmypid();
@mkdir($root, 0700, true);
$beat = "$root/beat.log";
$stop = "$root/stop";

$cleanup = function () use ($root, $beat, $stop) {
    @unlink($stop);
    @unlink($beat);
    @unlink("$root/unrelated.php");
    @unlink("$root/s.php");
    @rmdir($root);
};

/* Stands in for the reused pid. It writes a heartbeat until the test is over.
 * The stop file makes it kill itself, as the back-to-back survivor test does:
 * nothing else would end it. */
file_put_contents("$root/unrelated.php", <<<PHP
<?php
error_reporting(0);
for (;;) {
    if (file_exists('{$stop}')) {
        exec('kill -9 ' . getmypid());
    }
    @file_put_contents('{$beat}', "x", FILE_APPEND);
    usleep(20000);
}
PHP);

/* The supervised script of [s] runs until the test ends. The copy must start
 * before the master retires a survivor, and it must not exit by itself. */
file_put_contents("$root/s.php", <<<PHP
<?php
error_reporting(0);
for (;;) {
    if (file_exists('{$stop}')) {
        exec('kill -9 ' . getmypid());
    }
    usleep(20000);
}
PHP);

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice

[s]
pool.type = supervisor
supervisor.script = $root/s.php
supervisor.processes = 1
supervisor.restart = never
supervisor.stop_timeout = 1
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

/* Start the unrelated process first, so its pid is alive when the master
 * reads FPMNG_RELOAD_SURVIVORS. nohup and & detach it from this process, and
 * echo $! prints its pid (nohup execs PHP, so the pid is the PHP process). */
$unrelatedPid = (int) trim((string) shell_exec('nohup ' . escapeshellarg(PHP_BINARY) . ' '
    . escapeshellarg("$root/unrelated.php") . ' >/dev/null 2>&1 & echo $!'));
check($unrelatedPid > 1, 'the unrelated process did not start');
$deadline = time() + 15;
while (time() < $deadline && !is_file($beat)) {
    usleep(100000);
}
check(is_file($beat), 'the unrelated process did not write its heartbeat');

/* Field 22 of /proc/<pid>/stat is the start time. It is counted after the last
 * ')', because the command name in field 2 may contain spaces and ')'. */
$stat = (string) file_get_contents("/proc/$unrelatedPid/stat");
$fields = explode(' ', substr($stat, strrpos($stat, ')') + 2));
$realStart = (int) $fields[19];
check($realStart > 0, 'the start time of the unrelated process is not readable');

$env = getenv();
$env['FPMNG_RELOAD_SURVIVORS'] = "s:$unrelatedPid:" . (time() + 3600) . ':' . ($realStart + 1);

$tester = new FPM\Tester($config, '<?php');
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false, false, [], [], $env);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $logFile = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);

    check(waitForLog($logFile, "reload survivor pid $unrelatedPid from the previous generation is not the process it was when spared", 20),
        'the master did not refuse the pid of the unrelated process');
    $log = (string) @file_get_contents($logFile);
    check(!str_contains($log, "reload survivor pid $unrelatedPid is still running"),
        'the master tracked the pid of the unrelated process');

    /* The heartbeat must keep growing after the refusal. Had the master
     * signalled the process, it would have stopped before this point. */
    clearstatcache();
    $before = (int) @filesize($beat);
    usleep(2000000);
    clearstatcache();
    $after = (int) @filesize($beat);
    check($after > $before, "the unrelated process was signalled: heartbeat stopped at $before bytes");

    echo "unrelated pid not signalled: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    /* The unrelated process and the supervised copy check the stop file every
     * 20 ms. It must see the file before cleanup() removes it. */
    @touch($stop);
    usleep(500000);
    $cleanup();
}
?>
--EXPECT--
unrelated pid not signalled: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
