--TEST--
fpm-ng: a selective reload whose execvp() fails after the configuration check passed leaves no spared worker behind (issues #661, #690)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (!is_executable('/bin/bash')) {
    die("skip /bin/bash is needed for exec -a");
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #690: the old master has detached the unchanged pool's workers and listed
 * them in FPMNG_SELECTIVE_RELOAD_SURVIVORS when fpm_pctl_exec() calls execvp(). If
 * that execvp() fails, the master logs "failed to reload: execvp() failed" and
 * exits, and no master adopts the workers. fpm_pctl_exec() discards them first.
 *
 * Issue #661 refuses a reload whose check cannot exec the binary, so the #690
 * path is reached only when the binary disappears between the check and the exec.
 * This test makes that happen. argv[0] of the master is a shell script that
 * exits 0 for the check's "-t" and removes itself. The check therefore passes,
 * and the reload's own execvp(argv[0]) fails with ENOENT.
 *
 * The master is started through bash, which sets argv[0] to the script with
 * "exec -a". The binary itself is never removed, so the Tester can use it. */
$root = sys_get_temp_dir() . '/fpmng-reload-sel-execafter-' . getmypid();
@mkdir($root, 0700, true);
$pidFile = "$root/worker.pid";
$log = "$root/error.log";
$script = "$root/php-fpm-ng-script";

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
if ($binary === false) {
    fail('cannot find the php-fpm-ng binary');
}
file_put_contents($script, <<<'SH'
#!/bin/sh
# The configuration check passes "-t". Remove this file, so that the reload's
# own execvp() of the same path fails.
for arg in "$@"; do
	if [ "$arg" = "-t" ]; then
		rm -f "$0"
		exit 0
	fi
done
exit 1
SH);
chmod($script, 0755);
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
    $proc = proc_open(['/bin/bash', '-c', 'exec -a "$0" "$@"', $script, $binary, '-n', '-y', "$root/fpm.conf", '-F'],
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
    if (!is_resource($proc)) {
        fail('cannot start the master');
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

    exec("kill -USR2 $masterPid");

    if (!wait_gone($masterPid, 30)) {
        fail("master $masterPid did not exit after the failed execvp()\n" . (string) @file_get_contents($log));
    }
    $logText = (string) @file_get_contents($log);
    if (str_contains($logText, 'reload refused')) {
        fail("the check refused the reload, so the exec was never reached\n$logText");
    }
    if (!str_contains($logText, 'failed to reload: execvp() failed')) {
        fail("the reload did not reach a failed execvp()\n$logText");
    }
    echo "master exited after failed execvp: ok\n";

    if (!wait_gone($workerPid, 10)) {
        fail("spared worker $workerPid outlived the master whose execvp() failed");
    }
    echo "spared worker gone: ok\n";

    $probe = @stream_socket_server("tcp://$keep", $errno, $error);
    if (!$probe) {
        fail("the listening address $keep is still held: $error");
    }
    fclose($probe);
    echo "port free: ok\n";

    echo "Done\n";
} finally {
    foreach ([$workerPid ?? 0, $masterPid ?? 0] as $pid) {
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
master exited after failed execvp: ok
spared worker gone: ok
port free: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
