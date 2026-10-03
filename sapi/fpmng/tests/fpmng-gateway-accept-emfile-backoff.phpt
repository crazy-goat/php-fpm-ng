--TEST--
fpm-ng: a gateway out of file descriptors does not spin on accept() and recovers when they are freed (issue #687)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
if (!is_dir('/proc/self/fd')) {
    die("skip needs /proc");
}
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #687. The global rlimit_files caps every process of the master at 64
 * descriptors (pool.type = gateway has no rlimit_files of its own; a few
 * descriptors go to the listener, the event base, the log and the FastCGI side).
 * Idle clients are opened until accept() starts failing with EMFILE; the
 * kernel queue keeps the rest, so the listening socket stays readable. Without
 * an evconnlistener error callback libevent only logs each failed accept()
 * to stderr. In production that is a busy loop on one core. In this harness
 * stderr is a pipe nobody drains, so the unfixed gateway blocks in write()
 * and uses no CPU: the CPU check below therefore only catches a callback that
 * does not pause, and a missing callback is caught by the log assertion (the
 * "pausing accept" WARNING comes from the callback). Afterwards the
 * clients are closed and a request must be answered. */

$docroot = sys_get_temp_dir() . '/fpmng-gw-emfile-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo "ok";');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
rlimit_files = 64
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.front_controller = /index.php
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR[web]}}
chdir = $docroot
pm = static
pm.max_children = 1
EOT;

function gatewayPid(int $master): int
{
    foreach (glob('/proc/[0-9]*/stat') as $stat) {
        $raw = @file_get_contents($stat);
        if ($raw === false || !preg_match('/^(\d+) \((.*)\) \S (\d+) /s', $raw, $m)) {
            continue;
        }
        if ((int) $m[3] !== $master) {
            continue;
        }
        $cmd = @file_get_contents(dirname($stat) . '/cmdline');
        if ($cmd !== false && str_contains($cmd, 'http gateway gw')) {
            return (int) $m[1];
        }
    }
    return 0;
}

function cpuTicks(int $pid): int
{
    $raw = file_get_contents("/proc/$pid/stat");
    $f = explode(' ', substr($raw, strrpos($raw, ')') + 2));
    return (int) $f[11] + (int) $f[12]; // utime + stime (fields 14 and 15)
}

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
$clients = [];
try {
    $tester->start();
    $tester->expectLogStartNotices();

    [$host, $port] = explode(':', $tester->getAddr('ipv4', '[http]'));
    $gw = gatewayPid($tester->getPid());
    if ($gw === 0) {
        echo "FAIL: gateway process not found\n";
        exit(1);
    }
    if (!preg_match('/Max open files\s+(\d+)/', file_get_contents("/proc/$gw/limits"), $m) || (int) $m[1] !== 64) {
        echo "FAIL: gateway did not get rlimit_files = 64: " . ($m[1] ?? '?') . "\n";
        exit(1);
    }

    for ($i = 0; $i < 200; $i++) {
        $fp = @fsockopen($host, (int) $port, $errno, $errstr, 2);
        if (!$fp) {
            break;
        }
        $clients[] = $fp;
    }
    usleep(500000);
    $open = count(scandir("/proc/$gw/fd")) - 2;
    if ($open < 55) {
        echo "FAIL: the gateway did not run out of descriptors ($open open, " . count($clients) . " clients)\n";
        exit(1);
    }
    echo "exhausted: yes\n";

    $before = cpuTicks($gw);
    sleep(2);
    $used = cpuTicks($gw) - $before;
    // A spinning accept loop uses about 200 ticks in 2 s; a poll every 100 ms uses a few.
    if ($used > 40) {
        echo "FAIL: the gateway used $used ticks of CPU in 2 s while out of descriptors (busy loop)\n";
        exit(1);
    }
    echo "idle-while-exhausted: ok\n";

    // Fails with a clear message when the error callback is missing.
    $tester->expectLogPattern('/WARNING: \[pool gw\] http: accept\(\) on the main listener failed: .*; pausing accept for 100 ms/', true, 5);
    echo "backoff-logged: ok\n";

    foreach ($clients as $fp) {
        fclose($fp);
    }
    $clients = [];

    $body = '';
    for ($try = 0; $try < 20 && !preg_match('/^HTTP\/1\.1 200 .*\r\n\r\n(2\r\n)?ok/s', $body); $try++) {
        usleep(200000);
        $fp = @fsockopen($host, (int) $port, $errno, $errstr, 2);
        if (!$fp) {
            continue;
        }
        stream_set_timeout($fp, 3);
        fwrite($fp, "GET / HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
        $body = stream_get_contents($fp);
        fclose($fp);
    }
    if (!preg_match('/^HTTP\/1\.1 200 .*\r\n\r\n(2\r\n)?ok/s', $body)) {
        echo "FAIL: no answer after the descriptors were freed:\n$body\n";
        exit(1);
    }
    echo "recovered: ok\n";
    echo "Done\n";
} finally {
    foreach ($clients as $fp) {
        fclose($fp);
    }
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink($docroot . '/index.php');
    @rmdir($docroot);
}
?>
--EXPECT--
exhausted: yes
idle-while-exhausted: ok
backoff-logged: ok
recovered: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
