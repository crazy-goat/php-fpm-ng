--TEST--
fpm-ng: a reload survivor is retired under daemonize = yes, where the new master is not its parent (issue #891)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('supervisor');
if (!is_readable('/proc/self/stat')) {
    die('skip the survivor is observed through /proc (Linux only)');
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Reload A of a daemonized master spares one copy of supervisor pool [s] as a
 * survivor (issue #329). The exec'd image forks again under daemonize = yes
 * (third_party/php-src/sapi/fpm/fpm/fpm_unix.c:618), so the survivor is not a
 * child of the new master. Before issue #891 the new master identified the
 * survivor by parentage (waitpid), which failed for every such pid: the
 * survivor was refused and never retired, and it kept running with its copy
 * of the pool. The new master now identifies it by its start time, so it
 * must track the survivor and retire it once the replacement copy has started.
 *
 * The test checks the retirement through /proc: the survivor pid must be
 * gone, or a zombie that init has not reaped yet. */
$root = sys_get_temp_dir() . '/fpmng-survivor-daemonize-' . getmypid();
@mkdir($root, 0700, true);
$alive = "$root/alive.log";
$stop = "$root/stop";

$cleanup = function () use ($root, $alive, $stop) {
    @unlink($stop);
    @unlink($alive);
    @unlink("$root/s.php");
    @rmdir($root);
};

/* Writes its pid to $alive while it runs. The stop file ends it: a survivor
 * that no master tracks is not stopped by anything else. */
file_put_contents("$root/s.php", <<<PHP
<?php
error_reporting(0);
for (;;) {
    if (file_exists('{$stop}')) {
        exec('kill -9 ' . getmypid());
    }
    @file_put_contents('{$alive}', getmypid() . "\\n", FILE_APPEND);
    usleep(20000);
}
PHP);

$config = function (int $processes) use ($root) {
    return <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
reload.selective = yes

[s]
pool.type = supervisor
supervisor.script = $root/s.php
supervisor.processes = $processes
supervisor.restart = never
supervisor.stop_timeout = 1
EOT;
};

function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function waitForLog(string $file, string $pattern, int $seconds): ?array
{
    $deadline = time() + $seconds;
    while (time() < $deadline) {
        if (preg_match($pattern, (string) @file_get_contents($file), $m) === 1) {
            return $m;
        }
        usleep(100000);
    }
    return null;
}

/* True once the pid is gone, or a zombie that init has not reaped yet. */
function isRetired(int $pid): bool
{
    $stat = @file_get_contents("/proc/$pid/stat");
    if ($stat === false) {
        return true;
    }
    $fields = explode(' ', substr($stat, strrpos($stat, ')') + 2));
    return $fields[0] === 'Z';
}

$tester = new FPM\Tester($config(2), '<?php');
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false, true);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $logFile = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);

    /* Both copies of [s] are running before the reload. */
    $deadline = time() + 15;
    $pidsSeen = [];
    while (time() < $deadline && count($pidsSeen) < 2) {
        foreach (explode("\n", (string) @file_get_contents($alive)) as $line) {
            if (trim($line) !== '') {
                $pidsSeen[trim($line)] = true;
            }
        }
        usleep(100000);
    }
    check(count($pidsSeen) >= 2, 'before the reload: pool [s] did not start two copies');

    $tester->reload($config(3));

    $m = waitForLog($logFile, '/reload survivor pid (\d+) is still running the previous/', 20);
    check($m !== null, 'the new master did not track the survivor under daemonize');
    $survivor = (int) $m[1];

    $m = waitForLog($logFile, "/retiring reload survivor pid $survivor /", 20);
    check($m !== null, "the new master did not retire survivor pid $survivor");

    $gone = false;
    $deadline = time() + 10;
    while (time() < $deadline) {
        if (isRetired($survivor)) {
            $gone = true;
            break;
        }
        usleep(100000);
    }
    check($gone, "survivor pid $survivor is still running after it was retired");

    echo "daemonized survivor retired: ok\n";
    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    /* The survivor checks the stop file every 20 ms. It must see the file
     * before cleanup() removes it. */
    @touch($stop);
    usleep(500000);
    $cleanup();
}
?>
--EXPECT--
daemonized survivor retired: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
