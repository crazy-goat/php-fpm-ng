--TEST--
fpm-ng: a rolling reload whose new master fails init does not orphan the spared supervisor copy (issue #831)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #831. A supervisor pool keeps one copy of its script running across a
 * reload (issue #329): the old master spares that copy and hands its pid to
 * the exec'd master in FPMNG_RELOAD_SURVIVORS. The pool's init_main() takes
 * the entry, and only then does a generation track the copy. If the new master
 * dies before any pool takes it, nothing bounds the copy: the script runs on
 * with no master and no supervisor left to stop it.
 *
 * The failure is forced the same way as in fpmng-reload-selective-failed-init.phpt:
 * the reload moves the http-direct pool [change] to an address this test
 * process holds, so the new master's bind() fails in fpm_sockets_init_main(),
 * before any pool's init_main(). The fix is
 * fpm_pool_supervisor_reload_survivors_discard_unadopted(), called where the
 * master gives up. Without it the spared copy outlives the master and the
 * assertions below fail.
 *
 * reload.selective is off: the old master's full reload fans out to every
 * pool, and the rolling spare applies to the supervisor pool [sup] whose
 * configuration does not change. The master's pid does not change across the
 * reload under -F (tester.inc passes it), so the pid file is the one to watch. */
$work = sys_get_temp_dir() . '/fpmng-surv-initfail-' . getmypid();
@mkdir($work, 0700, true);
$aliveFile = "$work/alive.log";
@unlink($aliveFile);
file_put_contents("$work/front.php", "<?php\necho 'ok';\n");

/* Each copy appends its own pid once, then stays alive without doing work. */
$script = <<<PHP
<?php
file_put_contents('{$aliveFile}', getmypid() . "\\n", FILE_APPEND);
for (;;) {
    usleep(100000);
}
PHP;
file_put_contents("$work/loop.php", $script);

$section = function (string $changeListen) use ($work): string {
    return <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
[sup]
pool.type = supervisor
supervisor.script = $work/loop.php
supervisor.processes = 2
supervisor.restart = always
supervisor.restart_delay = 1
supervisor.stop_timeout = 2
[change]
listen = $changeListen
pool.type = http-direct
pm = static
pm.max_children = 1
chdir = $work
http.front_controller = /front.php
CFG;
};

function fail(string $what): void
{
    throw new RuntimeException($what);
}

/* A zombie counts as gone: the master is this test's proc_open() child and only
 * proc_close() reaps it. The spared copy is reparented once the master exits. */
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

$tester = new FPM\Tester($section('{{ADDR[change]}}'), $script);
$logFile = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
@unlink($logFile);
/* Held for the whole test: the address the reloaded master is sent to. */
$hog = null;
$masterPid = 0;
$survivor = 0;

try {
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();

    $pidFileMaster = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_PID);
    $masterPid = (int) trim((string) @file_get_contents($pidFileMaster));
    if ($masterPid <= 1) {
        fail('no master pid');
    }

    /* The spare needs two running copies of the pool. */
    $deadline = microtime(true) + 15;
    do {
        $pids = array_unique(array_filter(array_map('intval', explode("\n", (string) @file_get_contents($aliveFile)))));
        if (count($pids) >= 2) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    if (count($pids) < 2) {
        fail('the supervisor pool did not start two copies before the reload');
    }
    echo "supervisor running: ok\n";

    /* An address this process holds, so the new master's bind() fails. Port 0
     * lets the kernel pick a free one, and stream_socket_get_name() reads it
     * back -- nothing is assumed about the port number. */
    $hog = stream_socket_server('tcp://127.0.0.1:0', $errno, $error);
    if (!$hog) {
        fail("cannot occupy a listening address: $error");
    }
    $occupied = (string) stream_socket_get_name($hog, false);
    if ($occupied === '') {
        fail('cannot read back the occupied address');
    }

    $tester->reload($section($occupied));

    /* The old master logs the copy it spares before it execs, so the pid is
     * known even though the new master never reaches the pool. */
    $deadline = microtime(true) + 20;
    do {
        if (preg_match('/sparing child (\d+) from this reload/', (string) @file_get_contents($logFile), $m)) {
            $survivor = (int) $m[1];
            break;
        }
        usleep(50000);
    } while (microtime(true) < $deadline);
    if ($survivor <= 1) {
        fail("the old master did not spare a copy\n" . (string) @file_get_contents($logFile));
    }
    echo "copy spared: ok\n";

    if (!wait_gone($masterPid, 30)) {
        fail("master $masterPid did not exit after the reload that cannot initialise");
    }
    echo "master exited: ok\n";

    /* The survivor's own watchdog ends it after supervisor.stop_timeout (2 s)
     * once the master has sent it SIGTERM. 10 s leaves room for a slow box. */
    if (!wait_gone($survivor, 10)) {
        fail("spared copy $survivor outlived the master that failed to initialise\n"
            . (string) @file_get_contents($logFile));
    }
    echo "survivor gone: ok\n";

    $logText = (string) @file_get_contents($logFile);
    if (!str_contains($logText, "survivor pid $survivor; sending it SIGTERM")) {
        fail("no WARNING that the master signalled survivor $survivor\n$logText");
    }
    if (!str_contains($logText, 'issue #831')) {
        fail("the WARNING does not name issue #831\n$logText");
    }
    echo "warning logged: ok\n";

    echo "Done\n";
} finally {
    if ($hog) {
        fclose($hog);
    }
    /* By pid and only these: the test must not leave a stray behind when it fails. */
    foreach ([$survivor, $masterPid] as $pid) {
        if ($pid > 1 && pid_alive($pid)) {
            exec("kill -9 $pid 2>/dev/null");
        }
    }
    foreach (glob("$work/*") ?: [] as $f) {
        @unlink($f);
    }
    @rmdir($work);
}

?>
--EXPECT--
supervisor running: ok
copy spared: ok
master exited: ok
survivor gone: ok
warning logged: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
