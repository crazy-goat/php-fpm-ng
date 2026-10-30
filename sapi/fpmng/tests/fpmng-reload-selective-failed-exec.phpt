--TEST--
fpm-ng: a reload whose configuration check cannot exec the binary is refused and the service keeps running (issues #661, #690)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #690 made a failed execvp() of a reload clean up after itself, and the
 * master exited. Issue #661 changes the outcome. The configuration check runs
 * the same argv with -t before any gateway or worker is drained, and an exec
 * failure there refuses the reload. The running master keeps its pool, its
 * listening socket and its workers, and the next SIGUSR2 tries again.
 *
 * FPM\Tester always starts the binary it finds itself, which is shared with
 * other tests and cannot be removed, so this test starts a PRIVATE COPY of the
 * binary directly and unlinks the copy before the reload. execvp(saved_argv[0])
 * with the absolute path of the copy then fails with ENOENT. The Tester is used
 * only to hand out free addresses. */
$root = sys_get_temp_dir() . '/fpmng-reload-sel-failexec-' . getmypid();
@mkdir($root, 0700, true);
$pidFile = "$root/worker.pid";
$log = "$root/error.log";
$copy = "$root/php-fpm-ng-copy";

file_put_contents("$root/front.php", <<<'PHP'
<?php
file_put_contents(getenv('FPMNG_WORKER_PIDFILE'), (string) getmypid());
echo 'ok';
PHP);

function fail(string $what): void
{
    throw new RuntimeException($what);
}

function pid_alive(int $pid): bool
{
    if ($pid <= 1 || !is_dir("/proc/$pid")) {
        return false;
    }
    $stat = @file_get_contents("/proc/$pid/stat");
    $close = $stat === false ? false : strrpos($stat, ')');
    return $close === false || ($stat[$close + 2] ?? '') !== 'Z';
}

/* The pids of the direct children of $parent, read from /proc. */
function child_pids(int $parent): array
{
    $pids = [];
    foreach (glob('/proc/[0-9]*/stat') ?: [] as $stat) {
        $text = @file_get_contents($stat);
        if ($text === false) {
            continue;
        }
        $fields = explode(' ', substr($text, strrpos($text, ')') + 2));
        if ((int) ($fields[1] ?? 0) === $parent) {
            $pids[] = (int) basename(dirname($stat));
        }
    }
    return $pids;
}

function request(string $address): bool
{
    $fp = @stream_socket_client("tcp://$address", $errno, $error, 2);
    if (!$fp) {
        return false;
    }
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
    return str_contains($body, 'ok');
}

$tester = new FPM\Tester('[global]', '<?php');
$keep = $tester->getAddr('ipv4', '[keep]');
$binary = FPM\Tester::findExecutable();
if ($binary === false || !copy($binary, $copy) || !chmod($copy, 0755)) {
    fail('cannot copy the php-fpm-ng binary');
}
file_put_contents("$root/fpm.conf", <<<CFG
[global]
error_log = $log
pid = $root/fpm.pid
log_level = notice
reload.selective = yes
[keep]
listen = $keep
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
env[FPMNG_WORKER_PIDFILE] = $pidFile
CFG);

$proc = null;
try {
    $proc = proc_open([$copy, '-n', '-y', "$root/fpm.conf", '-F'],
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
    if (!is_resource($proc)) {
        fail('cannot start the copy');
    }
    $masterPid = (int) proc_get_status($proc)['pid'];

    $deadline = time() + 20;
    while (!request($keep)) {
        if (time() > $deadline) {
            fail("the pool never answered\n" . (string) @file_get_contents($log));
        }
        usleep(100000);
    }
    $workerPid = (int) @file_get_contents($pidFile);
    if (!pid_alive($workerPid)) {
        fail("worker pid '$workerPid' is not alive before the reload");
    }
    echo "worker running: ok\n";

    unlink($copy);
    exec("kill -USR2 $masterPid");

    $deadline = time() + 30;
    while (!str_contains((string) @file_get_contents($log), 'reload refused: the configuration check cannot run')) {
        if (time() > $deadline) {
            fail("the reload was not refused\n" . (string) @file_get_contents($log));
        }
        usleep(100000);
    }
    echo "reload refused: ok\n";

    if (!pid_alive($masterPid)) {
        fail("master $masterPid exited after the refused reload");
    }
    echo "master still running: ok\n";

    if (!request($keep)) {
        fail('the service stopped answering after the refused reload');
    }
    echo "service still answers: ok\n";

    $probe = @stream_socket_server("tcp://$keep", $errno, $error);
    if ($probe) {
        fclose($probe);
        fail("the listening address $keep was released by the refused reload");
    }
    echo "listener still held: ok\n";

    echo "Done\n";
} finally {
    /* Stop the copy as a service is stopped: SIGTERM to the master, which stops
     * its workers. Then SIGKILL whatever is still alive. A worker that never
     * answered has no pid in $pidFile, so the children come from /proc. A worker
     * left behind would keep the lane port and fail the next test. */
    $victims = [];
    if (isset($masterPid)) {
        $victims = child_pids($masterPid);
        exec("kill -TERM $masterPid 2>/dev/null");
        $stop = microtime(true) + 10;
        while (pid_alive($masterPid) && microtime(true) < $stop) {
            usleep(50000);
        }
        $victims[] = $masterPid;
    }
    if (isset($workerPid)) {
        $victims[] = $workerPid;
    }
    foreach ($victims as $pid) {
        if ($pid > 1 && pid_alive($pid)) {
            exec("kill -9 $pid 2>/dev/null");
        }
    }
    if (is_resource($proc)) {
        proc_close($proc);
    }
    foreach (glob("$root/*") ?: [] as $f) {
        @unlink($f);
    }
    @rmdir($root);
}

?>
--EXPECT--
worker running: ok
reload refused: ok
master still running: ok
service still answers: ok
listener still held: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
