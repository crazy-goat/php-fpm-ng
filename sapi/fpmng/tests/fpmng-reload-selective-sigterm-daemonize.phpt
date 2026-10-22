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

/* Not posix_kill(): the harness runs tests under `php -n`, where ext/posix
 * (a shared module on Ubuntu) is not loaded. */
function killHard(int $pid): void
{
    exec('kill -KILL ' . $pid . ' 2>/dev/null');
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

    /* While the master runs: an adopted worker that exits on its own must
     * be retired and replaced (a pm = static pool refills). The master gets
     * no SIGCHLD for it. */
    $victim = $workers[0];
    killHard($victim);
    check(
        waitForLog($logFile, "/adopted child $victim is gone/", 15),
        "the sweep did not retire adopted worker $victim"
    );
    $deadline = time() + 15;
    while (time() < $deadline && count($fresh = childrenOf($master)) < 1) {
        usleep(100000);
    }
    check(count($fresh) === 1, 'the pool did not fork a replacement for the retired worker');
    check(
        waitForLog($logFile, '/child ' . $fresh[0] . ' started/', 5),
        'the replacement is not logged as started'
    );
    $workers = array_merge($workers, $fresh);

    $tester->signal('TERM', $master);

    $deadline = time() + 15;
    while (time() < $deadline && !isGone($master)) {
        usleep(100000);
    }
    check(isGone($master), "master $master still runs 15 s after SIGTERM");
    foreach ($workers as $pid) {
        check(isGone($pid), "worker $pid still runs after the master exited");
    }

    /* Nothing may keep the pool's address after the stop. */
    $fp = @stream_socket_client('tcp://' . $tester->getAddr('ipv4', 'a'), $errno, $errstr, 2);
    check($fp === false, 'the pool address still accepts connections after the stop');

    echo "replacement after an adopted exit: ok\n";
    echo "master exited after SIGTERM: ok\n";
    echo "Done\n";
} finally {
    /* A failed check leaves a daemonized master that nothing else stops. */
    $filePid = (int) trim((string) @file_get_contents($tester->getPrefixedFile('pid')));
    foreach (array_merge([$master, $filePid], $workers) as $pid) {
        if ($pid > 1 && !isGone($pid)) {
            killHard($pid);
        }
    }
    $tester->close();
}
?>
--EXPECT--
replacement after an adopted exit: ok
master exited after SIGTERM: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
