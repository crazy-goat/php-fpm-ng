--TEST--
fpm-ng: a new daemonized master killed before it adopts the spared workers leaves none behind (issue #829)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_no_fault_inject();
if (!is_readable('/proc/self/stat')) {
    die('skip the workers are observed through /proc (Linux only)');
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #829, the daemonize = yes case of fpmng-reload-selective-guard-sigkill.phpt.
 * Under daemonize = yes the exec'd master forks again (fpm_unix.c:618), so the
 * new master is another pid than the old one. The old master armed the guard
 * before the exec, so the guard is a child of the OLD pid: the test finds it by
 * that pid.
 *
 * Pool [a] is byte-identical in both configs, so the reload spares both workers.
 * The new master is held before it adopts them (selective_adopt_hold). It has no
 * child of its own at that point: the spared workers belong to the pre-fork
 * parent, which has exited. The test kills the new master with SIGKILL and
 * requires the guard to stop both workers.
 *
 * Mutation: delete the fpm_reload_selective_guard_arm() call in fpm_pctl_exec()
 * and both spared workers outlive the killed master. */
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

function wait_log(string $file, string $pattern, int $seconds): bool
{
    $deadline = time() + $seconds;
    do {
        if (preg_match($pattern, (string) @file_get_contents($file)) === 1) {
            return true;
        }
        usleep(100000);
    } while (time() < $deadline);
    return false;
}

/* The daemonized master writes its pid file after its start notices, and a
 * reload briefly replaces it: poll until it holds a pid other than $not. */
function master_pid(FPM\Tester $tester, int $not = 0): int
{
    $file = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_PID);
    $deadline = time() + 20;
    do {
        $pid = (int) trim((string) @file_get_contents($file));
        if ($pid > 1 && $pid !== $not) {
            return $pid;
        }
        usleep(100000);
    } while (time() < $deadline);
    throw new RuntimeException("no master pid in $file other than $not");
}

/* True when the pid is gone, or a zombie that init has not reaped yet. */
function is_gone(int $pid): bool
{
    $stat = @file_get_contents("/proc/$pid/stat");
    if ($stat === false) {
        return true;
    }
    $fields = explode(' ', substr($stat, strrpos($stat, ')') + 2));
    return $fields[0] === 'Z';
}

function wait_gone(int $pid, int $seconds): bool
{
    $deadline = time() + $seconds;
    do {
        if (is_gone($pid)) {
            return true;
        }
        usleep(100000);
    } while (time() < $deadline);
    return false;
}

/* Pids whose parent is $ppid, read from /proc. Zombies are left out. */
function children_of(int $ppid): array
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

/* The pid of the selective reload guard that master $master armed. The guard is
 * not a child of the master: its parent exits at once, so init adopts it. 0 when
 * none runs. */
function guard_pid(int $master): int
{
    foreach (glob('/proc/[0-9]*/cmdline') as $file) {
        $cmd = @file_get_contents($file);
        if ($cmd !== false && preg_match('/selective reload guard of master (\d+)/', $cmd, $m) === 1 && (int) $m[1] === $master) {
            return (int) explode('/', $file)[2];
        }
    }
    return 0;
}

/* Not posix_kill(): the harness runs tests under `php -n`, where ext/posix
 * (a shared module on Ubuntu) is not loaded. */
function killHard(int $pid): void
{
    exec('kill -KILL ' . $pid . ' 2>/dev/null');
}

$tester = new FPM\Tester($config, '<?php');
$oldMaster = 0;
$master = 0;
$guard = 0;
$workers = [];
try {
    @unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    $tester->start([], false, true, [], [], array_merge(getenv(), [
        'FPMNG_FAULT' => 'selective_adopt_hold:ifenv=FPMNG_SELECTIVE_RELOAD_SURVIVORS',
    ]));
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $logFile = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);

    $oldMaster = master_pid($tester);
    $deadline = time() + 15;
    do {
        $workers = children_of($oldMaster);
        if (count($workers) >= 2) {
            break;
        }
        usleep(100000);
    } while (time() < $deadline);
    check(count($workers) === 2, 'before the reload: pool [a] did not start two workers');
    echo "workers running: ok\n";

    /* Identical config: both workers are spared and handed over. */
    $tester->reload();

    check(wait_log($logFile, "/fault injection: 'selective_adopt_hold'/", 20), 'the new master did not reach the adopt hold');
    check(
        preg_match('/issue #330: adopted/', (string) file_get_contents($logFile)) !== 1,
        'the new master adopted the spared workers before the hold, the test does not reproduce #829'
    );
    $master = master_pid($tester, $oldMaster);
    echo "held before adopt: ok\n";

    check(children_of($master) === [], 'the new master has a child during the hold, the spared workers are not its children');
    echo "no child of the new master: ok\n";

    $guard = guard_pid($oldMaster);

    killHard($master);

    foreach ($workers as $pid) {
        check(wait_gone($pid, 10), "spared worker $pid outlived the killed master: the guard did not stop it");
    }
    echo "spared workers gone: ok\n";

    check($guard > 1, "no selective reload guard ran for the old master $oldMaster before the kill");
    echo "guard running: ok\n";

    check(wait_gone($guard, 5), "the guard $guard did not exit after it stopped the carried workers");
    echo "guard exited: ok\n";

    check(
        wait_log($logFile, '/issue #829: .* sent SIGTERM to 2 child\(ren\)/', 5),
        'the guard did not log that it sent SIGTERM to both carried workers'
    );
    echo "guard logged the SIGTERM: ok\n";

    /* The pool address must be bindable again. */
    $probe = @stream_socket_server('tcp://' . $tester->getAddr('ipv4', 'a'), $errno, $error);
    check($probe !== false, 'the pool address is still held: ' . $error);
    fclose($probe);
    echo "port free: ok\n";

    echo "Done\n";
} finally {
    /* A failed check leaves processes that nothing else stops. By pid only. */
    $filePid = (int) trim((string) @file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_PID)));
    foreach (array_merge([$master, $filePid, $guard], $workers) as $pid) {
        if ($pid > 1 && !is_gone($pid)) {
            killHard($pid);
        }
    }
    $tester->close();
}
?>
--EXPECT--
workers running: ok
held before adopt: ok
no child of the new master: ok
spared workers gone: ok
guard running: ok
guard exited: ok
guard logged the SIGTERM: ok
port free: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
