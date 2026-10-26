--TEST--
fpm-ng: a new master killed before it adopts the spared workers leaves none behind (issue #829)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_no_fault_inject();
fpmng_skip_if_pool_type_unsupported('gateway');
if (!is_readable('/proc/self/stat')) {
    die('skip the workers are observed through /proc (Linux only)');
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #829. With reload.selective the old master hands the pids of an
 * unchanged pool's workers to the exec'd master in FPMNG_SELECTIVE_RELOAD_SURVIVORS.
 * The new master adopts them in fpm_reload_selective_adopt(). Until then the
 * #690 discard never runs for them: a master that is killed (SIGKILL, a crash)
 * does not reach fpm_pctl_exit(), so the spared worker keeps its listening socket
 * with no master to stop it.
 *
 * The guard (fpm_reload_selective_guard.c) covers that window. The old master
 * arms a pipe before the exec; the new master releases it after the adoption;
 * when every holder of the write end is gone, the guard SIGTERMs the carried pids.
 *
 * The test holds the new master before it adopts (the point
 * selective_adopt_hold, fpm_fault_inject.h), then kills it with SIGKILL. [gw]
 * and [change] come first in the config: their processes are forked in their
 * own init, before the hold of [keep], so the new master has a gateway and a
 * child of its own that were not carried. The guard does not cover them; the
 * test kills them at the end.
 *
 * The gateway matters for the write end of the pipe. A gateway forked by the
 * new master before the release inherits it, and it does not die with its
 * master. Unless fpm_reload_selective_guard_child_init() closes that copy in
 * the gateway (fpm_http_gw_proc.c), the guard waits for it and never fires. So
 * the test checks that the gateway exists before the kill.
 *
 * Mutation: delete the fpm_reload_selective_guard_arm() call in fpm_pctl_exec()
 * and the spared worker of [keep] outlives the killed master. Delete the
 * fpm_reload_selective_guard_child_init() call in fpm_http_gw_proc.c and the
 * same happens, through the gateway. */
$root = sys_get_temp_dir() . '/fpmng-reload-sel-guard-kill-' . getmypid();
@mkdir($root, 0700, true);
$pidFile = "$root/worker.pid";
@unlink($pidFile);
file_put_contents("$root/front.php", <<<'PHP'
<?php
file_put_contents(getenv('FPMNG_WORKER_PIDFILE'), (string) getmypid());
echo 'ok';
PHP);
@mkdir("$root/alt");

$section = function (string $changeChdir) use ($root, $pidFile): string {
    return <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
reload.selective = yes
[gw]
pool.type = gateway
listen = {{ADDR[gw]}}
chdir = $changeChdir
http.gateways = 1
http.route[change] = /
[change]
listen = {{ADDR[change]}}
pm = static
pm.max_children = 1
chdir = $changeChdir
[keep]
listen = {{ADDR[keep]}}
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
env[FPMNG_WORKER_PIDFILE] = $pidFile
CFG;
};

function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}

/* A zombie counts as gone: the master is this test's proc_open() child and only
 * proc_close() reaps it. */
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

/* A gateway process of pool [gw]: its title is set in fpm_http_gateway_run(). */
function is_gateway(int $pid): bool
{
    $cmd = @file_get_contents("/proc/$pid/cmdline");
    return $cmd !== false && str_contains($cmd, 'http gateway gw');
}

/* Not posix_kill(): the harness runs tests under `php -n`, where ext/posix
 * (a shared module on Ubuntu) is not loaded. */
function killHard(int $pid): void
{
    exec('kill -KILL ' . $pid . ' 2>/dev/null');
}

function request(string $address): void
{
    $fp = @stream_socket_client("tcp://$address", $errno, $error, 5);
    check((bool) $fp, "connect $address: $error");
    stream_set_timeout($fp, 10);
    fwrite($fp, "GET /app HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $body = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 8192);
        if ($chunk === false || $chunk === '') {
            break;
        }
        $body .= $chunk;
    }
    fclose($fp);
    check(str_contains($body, 'ok'), "no answer from $address: " . var_export($body, true));
}

$tester = new FPM\Tester($section($root), '<?php');
$keep = $tester->getAddr('ipv4', '[keep]');
$masterPid = 0;
$workerPid = 0;
$changePid = 0;
$gatewayPid = 0;
$guardPid = 0;

try {
    $tester->start([], false, false, [], [], array_merge(getenv(), [
        'FPMNG_FAULT' => 'selective_adopt_hold:ifenv=FPMNG_SELECTIVE_RELOAD_SURVIVORS',
    ]));
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $logFile = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);

    $masterPid = (int) trim((string) @file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_PID)));
    check($masterPid > 1, 'no master pid');

    request($keep);
    $workerPid = (int) @file_get_contents($pidFile);
    check(pid_alive($workerPid), "worker pid '$workerPid' is not alive before the reload");
    echo "worker running: ok\n";

    /* Only [change] differs, so [keep] is spared and its pid is handed over. */
    $tester->reload($section("$root/alt"));

    check(wait_log($logFile, "/fault injection: 'selective_adopt_hold'/", 20), 'the new master did not reach the adopt hold');
    check(
        preg_match('/issue #330: adopted/', (string) file_get_contents($logFile)) !== 1,
        'the new master adopted the spared worker before the hold, the test does not reproduce #829'
    );
    echo "held before adopt: ok\n";

    check(pid_alive($workerPid), "spared worker $workerPid is gone before the master was killed");
    $others = array_values(array_diff(children_of($masterPid), [$workerPid]));
    $gateways = array_values(array_filter($others, 'is_gateway'));
    check(count($gateways) === 1, 'expected one gateway of [gw] in the new master during the hold, found ' . count($gateways));
    $gatewayPid = $gateways[0];
    echo "gateway forked before the hold: ok\n";

    $others = array_values(array_diff($others, [$gatewayPid]));
    check(count($others) === 1, 'the changed pool [change] has no child of the new master during the hold');
    $changePid = $others[0];
    echo "changed pool child: ok\n";

    $guardPid = guard_pid($masterPid);

    killHard($masterPid);

    check(wait_gone($workerPid, 10), "spared worker $workerPid outlived the killed master: the guard did not stop it");
    echo "spared worker gone: ok\n";

    check($guardPid > 1, "no selective reload guard ran for master $masterPid before it was killed");
    echo "guard running: ok\n";

    check(wait_gone($guardPid, 5), "the guard $guardPid did not exit after it stopped the carried workers");
    echo "guard exited: ok\n";

    check(
        wait_log($logFile, '/issue #829: .* sent SIGTERM to 1 child\(ren\)/', 5),
        'the guard did not log that it sent SIGTERM to the carried worker'
    );
    echo "guard logged the SIGTERM: ok\n";

    /* The port is what the issue is about: it must be bindable again. */
    $probe = @stream_socket_server("tcp://$keep", $errno, $error);
    check($probe !== false, "the listening address $keep is still held: $error");
    fclose($probe);
    echo "port free: ok\n";

    echo "Done\n";
} finally {
    /* By pid and only these: the test must not leave a stray behind when it fails. */
    foreach ([$workerPid, $changePid, $gatewayPid, $guardPid, $masterPid] as $pid) {
        if ($pid > 1 && pid_alive($pid)) {
            killHard($pid);
        }
    }
    foreach (glob("$root/*") ?: [] as $f) {
        @unlink($f);
    }
    @rmdir("$root/alt");
    @rmdir($root);
}

?>
--EXPECT--
worker running: ok
held before adopt: ok
gateway forked before the hold: ok
changed pool child: ok
spared worker gone: ok
guard running: ok
guard exited: ok
guard logged the SIGTERM: ok
port free: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
