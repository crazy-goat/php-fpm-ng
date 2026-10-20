--TEST--
fpm-ng: SIGTERM after a sparing reload completes under daemonize = yes (issue #792)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (!is_readable('/proc/self/stat')) {
    die('skip the workers are observed through /proc (Linux only)');
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Under daemonize = yes the exec'd image of a reload forks again
 * (third_party/php-src/sapi/fpm/fpm/fpm_unix.c:618), so the new master is not
 * the parent of the workers that a selective reload spared. Their exit raised
 * no SIGCHLD in it, fpm_children_bury() never unlinked them,
 * fpm_globals.running_children never reached 0 and a SIGTERM never completed:
 * the master kept its sockets and re-sent SIGKILL to pids that were gone.
 *
 * Pool [a] is byte-identical in both configs, so the reload spares both
 * workers. The test then stops the master with SIGTERM and requires the
 * master to exit. */
$config = <<<'EOT'
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
reload.selective = yes
process_control_timeout = 2

[a]
listen = {{ADDR[a]}}
pm = static
pm.max_children = 2
EOT;

function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

function waitForLog(string $file, string $pattern, int $seconds): bool
{
    $deadline = time() + $seconds;
    while (time() < $deadline) {
        if (preg_match($pattern, (string) @file_get_contents($file)) === 1) {
            return true;
        }
        usleep(100000);
    }
    return false;
}

/* The daemonized master writes its pid file after its start notices, and a
 * reload briefly replaces it: poll until it holds a pid other than $not. */
function masterPid(FPM\Tester $tester, int $not = 0): int
{
    $file = $tester->getPrefixedFile("pid");
    $deadline = time() + 20;
    while (time() < $deadline) {
        $pid = (int) trim((string) @file_get_contents($file));
        if ($pid > 1 && $pid !== $not) {
            return $pid;
        }
        usleep(100000);
    }
    throw new RuntimeException("no master pid in $file other than $not");
}

/* True when the pid is gone, or a zombie that init has not reaped yet. */
function isGone(int $pid): bool
{
    $stat = @file_get_contents("/proc/$pid/stat");
    if ($stat === false) {
        return true;
    }
    $fields = explode(' ', substr($stat, strrpos($stat, ')') + 2));
    return $fields[0] === 'Z';
}

/* Pids whose parent is $ppid, read from /proc. */
function childrenOf(int $ppid): array
{
    $pids = [];
    foreach (glob('/proc/[0-9]*/stat') as $file) {
        $stat = @file_get_contents($file);
        if ($stat === false) {
            continue;
        }
        $fields = explode(' ', substr($stat, strrpos($stat, ')') + 2));
        if ((int) $fields[1] === $ppid && $fields[0] !== 'Z') {
            $pids[] = (int) explode('/', $file)[2];
        }
    }
    sort($pids);
    return $pids;
}

$tester = new FPM\Tester($config, '<?php');
$master = 0;
$workers = [];
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false, true);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $logFile = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);

    $oldMaster = masterPid($tester);
    $deadline = time() + 15;
    while (time() < $deadline && count($workers = childrenOf($oldMaster)) < 2) {
        usleep(100000);
    }
    check(count($workers) === 2, 'before the reload: pool [a] did not start two workers');

    $tester->reload();
    $master = masterPid($tester, $oldMaster);
    check(
        waitForLog($logFile, '/issue #330: adopted 2 child\(ren\)/', 20),
        'the new master did not adopt the two spared workers'
    );
    check(childrenOf($master) === [], 'the new master is the parent of the workers, the test does not reproduce #792');

    $tester->signal('TERM', $master);

    $deadline = time() + 15;
    while (time() < $deadline && !isGone($master)) {
        usleep(100000);
    }
    check(isGone($master), "master $master still runs 15 s after SIGTERM");
    foreach ($workers as $pid) {
        check(isGone($pid), "worker $pid still runs after the master exited");
    }

    echo "master exited after SIGTERM: ok\n";
    echo "Done\n";
} finally {
    /* A failed check leaves a daemonized master that nothing else stops. */
    $filePid = (int) trim((string) @file_get_contents($tester->getPrefixedFile('pid')));
    foreach (array_merge([$master, $filePid], $workers) as $pid) {
        if ($pid > 1 && !isGone($pid)) {
            posix_kill($pid, SIGKILL);
        }
    }
    $tester->close();
}
?>
--EXPECT--
master exited after SIGTERM: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
