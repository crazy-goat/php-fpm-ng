--TEST--
fpm-ng: a spared worker that the new master does not adopt is signalled after the initial fork loop, and the master keeps running (issue #690)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #690. A selective reload hands the pids of spared workers to the exec'd
 * master in FPMNG_SELECTIVE_RELOAD_SURVIVORS. fpm_reload_selective_adopt() takes
 * the entries whose pool name exists in the new configuration; the rest stay in
 * the environment. fpm_run() calls fpm_reload_selective_discard_unadopted() after
 * the initial fork loop (fpm.c, the call after fpm_children_create_initial()):
 * those leftovers are SIGTERMed there, and the master keeps running. Without that
 * call the leftover worker keeps its listening socket for the whole life of the
 * master, and nothing in this generation knows it exists.
 *
 * A real reload does not produce such a leftover. fpm_process_ctl.c spares a pool
 * only when fpm_conf_diff_pool_unchanged() is true, and that is false for a pool
 * the new configuration does not have (fpm_conf_diff.c, "new or removed pool").
 * So the survivor list is written by the test, the way a previous generation
 * would write it: an entry for pool "gone" that this configuration does not
 * define. The pid is a process this test starts itself, so only that process can
 * be signalled.
 *
 * The master's pid does not change under -F (tester.inc passes it), so the pid
 * file is the one to watch. */
$root = sys_get_temp_dir() . '/fpmng-reload-sel-discard-fork-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/front.php", <<<'PHP'
<?php
echo 'ok';
PHP);

$cfg = <<<CFG
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
CFG;

function fail(string $what): void
{
    throw new RuntimeException($what);
}

/* A zombie counts as gone: a process this test forks is reparented when its
 * shell exits, and may stay a zombie until init reaps it. */
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

$tester = new FPM\Tester($cfg, '<?php');
$keep = $tester->getAddr('ipv4', '[keep]');
$sleeperPid = 0;
$masterPid = 0;

try {
    exec('sleep 600 >/dev/null 2>&1 & echo $!', $out);
    $sleeperPid = (int) trim($out[0] ?? '');
    if (!pid_alive($sleeperPid)) {
        fail("cannot start the process that stands in for a spared worker");
    }

    $tester->start([], false, false, [], [], array_merge(getenv(), [
        'FPMNG_SELECTIVE_RELOAD_SURVIVORS' => "gone:$sleeperPid",
    ]));
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    $masterPid = (int) trim((string) @file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_PID)));
    if ($masterPid <= 1) {
        fail('no master pid');
    }

    /* Answering proves fpm_run() got past the initial fork loop, where the
     * discard call is. */
    request($keep);
    echo "master serving: ok\n";

    if (!wait_gone($sleeperPid, 10)) {
        fail("unadopted spared worker $sleeperPid is still running after the master started");
    }
    echo "unadopted spared worker signalled: ok\n";

    if (!pid_alive($masterPid)) {
        fail("master $masterPid exited, but it must keep running after the discard");
    }
    request($keep);
    echo "master still serving: ok\n";

    $log = (string) @file_get_contents($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
    if (!preg_match('/issue #690: this master did not adopt 1 child\(ren\) that a selective reload carried over; sent them SIGTERM/', $log)) {
        fail('the master did not log that it discarded the spared worker');
    }
    echo "discard logged: ok\n";

    echo "Done\n";
} finally {
    /* SIGTERM, not kill: the master's workers hold the listening socket and
     * would outlive a killed master, and the next test would not bind. */
    if ($masterPid > 1 && pid_alive($masterPid)) {
        $tester->terminate();
        wait_gone($masterPid, 30);
    }
    /* By pid and only these: the test must not leave a stray behind when it fails. */
    foreach ([$sleeperPid, $masterPid] as $pid) {
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
master serving: ok
unadopted spared worker signalled: ok
master still serving: ok
discard logged: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
