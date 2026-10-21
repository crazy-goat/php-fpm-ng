--TEST--
fpm-ng: a selective reload whose new master fails init_main() leaves no spared worker behind (issue #832)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_no_fault_inject();
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #832, the second path of the #690 discard. With reload.selective the old
 * master detaches an unchanged pool's workers and hands their pids to the exec'd
 * master in FPMNG_SELECTIVE_RELOAD_SURVIVORS. If type->init_main() of a pool then
 * fails in fpm_run(), after fpm_init() returned OK, fpm.c sets TERMINATING and
 * calls fpm_event_loop(1), which ends in fpm_pctl_exit(). That function runs
 * fpm_reload_selective_discard_unadopted(); without that call the worker of the
 * spared pool survives the master and keeps its port.
 *
 * fpmng-reload-selective-failed-init.phpt covers the failure of fpm_init() (a
 * bind that fails). No configuration reaches an init_main() failure: every cause
 * is a resource failure and validation refuses a bad pool earlier. So the
 * failure is injected: FPMNG_FAULT (fpm_fault_inject.h) fails the point
 * pool_type_init_main, and only in a process whose environment has
 * FPMNG_SELECTIVE_RELOAD_SURVIVORS. The first master has not got that variable,
 * so it starts; the exec'd master has it, so it fails. The reload changes pool
 * [change] only, so [keep] is spared.
 *
 * Mutation: delete the discard call in fpm_pctl_exit() and the worker of [keep]
 * outlives the master.
 *
 * The master's pid does not change across the reload under -F (tester.inc passes
 * it), so the pid file of the first generation is the one to watch. */
$root = sys_get_temp_dir() . '/fpmng-reload-sel-failinitmain-' . getmypid();
@mkdir($root, 0700, true);
$pidFile = "$root/worker.pid";
@unlink($pidFile);
file_put_contents("$root/front.php", <<<'PHP'
<?php
file_put_contents(getenv('FPMNG_WORKER_PIDFILE'), (string) getmypid());
echo 'ok';
PHP);
/* The changed [change] pool needs a front controller of its own: -t refuses a
 * pool whose chdir has none (issue #640). */
@mkdir("$root/alt");
copy("$root/front.php", "$root/alt/front.php");

$section = function (string $changeChdir) use ($root, $pidFile): string {
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
chdir = $changeChdir
http.front_controller = /front.php
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

$tester = new FPM\Tester($section($root), '<?php');
$keep = $tester->getAddr('ipv4', '[keep]');
$workerPid = 0;
$masterPid = 0;

try {
    $tester->start([], false, false, [], [], array_merge(getenv(), [
        'FPMNG_FAULT' => 'pool_type_init_main:ifenv=FPMNG_SELECTIVE_RELOAD_SURVIVORS',
    ]));
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    $masterPid = (int) trim((string) @file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_PID)));
    if ($masterPid <= 1) {
        fail('no master pid');
    }

    request($keep);
    $workerPid = (int) @file_get_contents($pidFile);
    if (!pid_alive($workerPid)) {
        fail("worker pid '$workerPid' is not alive before the reload");
    }
    echo "worker running: ok\n";

    /* Only [change] differs, so [keep] is spared and its pid is handed over. */
    $tester->reload($section("$root/alt"));

    if (!wait_gone($masterPid, 30)) {
        fail("master $masterPid did not exit after the reload whose init_main() fails");
    }
    echo "master exited: ok\n";

    if (!wait_gone($workerPid, 10)) {
        fail("spared worker $workerPid outlived the master that failed init_main()");
    }
    echo "spared worker gone: ok\n";

    $log = (string) @file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    if (!str_contains($log, "fault injection: 'pool_type_init_main' fails now")) {
        fail('the injected failure did not fire, so the test did not reach init_main()');
    }
    if (!preg_match('/failed to initialize pool type/', $log)) {
        fail('fpm_run() did not log the init_main() failure');
    }
    if (!preg_match('/issue #690: this master did not adopt 1 child\(ren\) that a selective reload carried over; sent them SIGTERM/', $log)) {
        fail('the new master did not log that it discarded the spared worker');
    }
    echo "discard logged: ok\n";

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
    foreach ([$workerPid, $masterPid] as $pid) {
        if ($pid > 1 && pid_alive($pid)) {
            exec("kill -KILL $pid 2>/dev/null");
        }
    }
    foreach (array_merge(glob("$root/*") ?: [], glob("$root/alt/*") ?: []) as $f) {
        @unlink($f);
    }
    @rmdir("$root/alt");
    @rmdir($root);
}

?>
--EXPECT--
worker running: ok
master exited: ok
spared worker gone: ok
discard logged: ok
port free: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
