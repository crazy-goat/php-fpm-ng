--TEST--
fpm-ng: a gateway listener survives a reload whose execvp() fails after the configuration check passed (issues #661, #925)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
if (!is_executable('/bin/bash')) {
    die("skip /bin/bash is needed for exec -a");
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #925 with the gateway, whose listener travels in FPMNG_HTTP_LISTENERS
 * (fpm_http_handoff.c) and not through the selective-reload environment. The
 * http-direct case is fpmng-reload-selective-exec-after-check.phpt.
 *
 * argv[0] of the master is a shell script that exits 0 for the check's "-t" and
 * removes itself, so the reload's own execvp(argv[0]) fails with ENOENT. The
 * master then retries through /proc/self/exe. The test expects, in this order,
 * the failed execvp() in the log, the takeover of the gateway listener by the
 * next generation, the same master pid, and an answer through the gateway. */
$root = sys_get_temp_dir() . '/fpmng-gw-exec-after-check-' . getmypid();
@mkdir($root, 0700, true);
$log = "$root/error.log";
$script = "$root/php-fpm-ng-script";

file_put_contents("$root/app.php", "<?php\nheader('Content-Length: 2');\necho 'ok';\n");

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

function gateway_ok(string $addr): bool
{
    $fp = @stream_socket_client("tcp://$addr", $errno, $error, 2);
    if (!$fp) {
        return false;
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET /app.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $body = (string) stream_get_contents($fp);
    fclose($fp);
    return str_starts_with($body, 'HTTP/1.1 200') && str_ends_with($body, 'ok');
}

$tester = new FPM\Tester('[global]', '<?php');
$http = $tester->getAddr('ipv4', '[gw]');
$fcgi = $tester->getAddr('ipv4', '[app]');
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
process_control_timeout = 10
[gw]
pool.type = gateway
listen = $http
chdir = $root
http.gateways = 1
http.route[app] = /
[app]
pool.type = fastcgi
listen = $fcgi
chdir = $root
pm = static
pm.max_children = 2
CFG);

$proc = null;
try {
    $proc = proc_open(['/bin/bash', '-c', 'exec -a "$0" "$@"', $script, $binary, '-n', '-y', "$root/fpm.conf", '-F'],
        [0 => ['file', '/dev/null', 'r'], 1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']], $pipes);
    if (!is_resource($proc)) {
        fail('cannot start the master');
    }
    $masterPid = (int) proc_get_status($proc)['pid'];

    $deadline = microtime(true) + 20;
    while (!gateway_ok($http)) {
        if (microtime(true) > $deadline) {
            fail("the gateway never answered\n" . (string) @file_get_contents($log));
        }
        usleep(100000);
    }
    echo "gateway answers: ok\n";

    exec("kill -USR2 $masterPid");

    $takeover = "took over the listener on $http";
    $deadline = microtime(true) + 30;
    do {
        $logText = (string) @file_get_contents($log);
        if (str_contains($logText, $takeover)) {
            break;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);
    if (str_contains($logText, 'reload refused')) {
        fail("the check refused the reload, so the exec was never reached\n$logText");
    }
    $failed = strpos($logText, 'failed to reload: execvp() failed');
    if ($failed === false) {
        fail("the reload did not reach a failed execvp()\n$logText");
    }
    $took = strpos($logText, $takeover);
    if ($took === false || $took < $failed) {
        fail("the next generation did not take the gateway listener over after the failed execvp()\n$logText");
    }
    echo "execvp failed, then the listener was taken over: ok\n";

    if (!pid_alive($masterPid)) {
        fail("master $masterPid exited after the failed execvp()\n$logText");
    }
    echo "same master pid: ok\n";

    $deadline = microtime(true) + 20;
    while (!gateway_ok($http)) {
        if (microtime(true) > $deadline) {
            fail("the gateway does not answer after the failed execvp()\n" . (string) @file_get_contents($log));
        }
        usleep(100000);
    }
    echo "gateway answers after the reload: ok\n";

    echo "Done\n";
} finally {
    if (isset($masterPid) && $masterPid > 1 && pid_alive($masterPid)) {
        exec("kill -TERM $masterPid 2>/dev/null");
        $deadline = microtime(true) + 12;
        while (pid_alive($masterPid) && microtime(true) < $deadline) {
            usleep(50000);
        }
        if (pid_alive($masterPid)) {
            exec("kill -9 $masterPid 2>/dev/null");
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
gateway answers: ok
execvp failed, then the listener was taken over: ok
same master pid: ok
gateway answers after the reload: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
