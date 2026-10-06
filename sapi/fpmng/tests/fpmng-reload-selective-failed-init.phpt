--TEST--
fpm-ng: a selective reload whose new master fails init leaves no spared worker behind (issue #690)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #690. With reload.selective the old master detaches an unchanged pool's
 * workers and hands their pids to the exec'd master in FPMNG_SELECTIVE_RELOAD_SURVIVORS
 * (fpm_reload_selective_spare_pool()). If the new master then dies during
 * initialisation, nobody adopts them: they are in no master's bookkeeping, nothing
 * notifies them that the master is gone, and they keep the pool's listening socket
 * -- so the port stays bound by a stray process after the failed reload.
 *
 * The failure is forced the plainest way: the reload rewrites only pool [change]
 * (the diff is by raw section text, docs/reload.md) and adds a directive the
 * parser refuses. The new master fails in fpm_conf_init_main(), before it ever
 * reaches fpm_children_create_initial() and fpm_reload_selective_adopt(), which is
 * the earliest place the pids can be lost. The fix is
 * fpm_reload_selective_discard_unadopted(), called where the master gives up.
 * Without it the worker of [keep] survives and the assertions below fail.
 *
 * The master's pid does not change across the reload under -F (tester.inc passes
 * it), so the pid file of the first generation is the one to watch. */
$root = sys_get_temp_dir() . '/fpmng-reload-sel-failinit-' . getmypid();
@mkdir($root, 0700, true);
$pidFile = "$root/worker.pid";
@unlink($pidFile);
file_put_contents("$root/front.php", <<<'PHP'
<?php
file_put_contents(getenv('FPMNG_WORKER_PIDFILE'), (string) getmypid());
echo 'ok';
PHP);

$section = function (string $extra) use ($root, $pidFile): string {
    return <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
reload.selective = yes
[keep]
listen = {{ADDR[keep]}}
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
env[FPMNG_WORKER_PIDFILE] = $pidFile
[change]
listen = {{ADDR[change]}}
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /front.php
$extra
CFG;
};

function fail(string $what): void
{
    throw new RuntimeException($what);
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

function request(string $address): void
{
    $fp = @stream_socket_client("tcp://$address", $errno, $error, 5);
    if (!$fp) {
        fail("connect $address: $error");
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
    if (!str_contains($body, 'ok')) {
        fail("no answer from $address: " . var_export($body, true));
    }
}

$tester = new FPM\Tester($section(''), '<?php');
$keep = $tester->getAddr('ipv4', '[keep]');

try {
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    $pidFileMaster = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_PID);
    $masterPid = (int) trim((string) @file_get_contents($pidFileMaster));
    if ($masterPid <= 1) {
        fail('no master pid');
    }

    request($keep);
    $workerPid = (int) @file_get_contents($pidFile);
    if (!pid_alive($workerPid)) {
        fail("worker pid '$workerPid' is not alive before the reload");
    }
    echo "worker running: ok\n";

    $tester->reload($section('no_such_directive_690 = 1'));

    if (!wait_gone($masterPid, 30)) {
        fail("master $masterPid did not exit after the reload that cannot initialise");
    }
    echo "master exited: ok\n";

    if (!wait_gone($workerPid, 10)) {
        fail("spared worker $workerPid outlived the master that failed to initialise");
    }
    echo "spared worker gone: ok\n";

    /* The port is what the issue is about: it must be bindable again. */
    $probe = @stream_socket_server("tcp://$keep", $errno, $error);
    if (!$probe) {
        fail("the listening address $keep is still held: $error");
    }
    fclose($probe);
    echo "port free: ok\n";

    echo "Done\n";
} finally {
    /* By pid and only these: the test must not leave a stray behind when it fails. */
    foreach ([$workerPid ?? 0, $masterPid ?? 0] as $pid) {
        if ($pid > 1 && pid_alive($pid)) {
            exec("kill -9 $pid 2>/dev/null");
        }
    }
    foreach (glob("$root/*") ?: [] as $f) {
        @unlink($f);
    }
    @rmdir($root);
}

?>
--EXPECT--
worker running: ok
master exited: ok
spared worker gone: ok
port free: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
