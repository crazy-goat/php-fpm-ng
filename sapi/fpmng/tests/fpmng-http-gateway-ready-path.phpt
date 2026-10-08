--TEST--
fpm-ng: http.ready_path answers 200 while the gateway serves, 503 to a probe pipelined behind a request during a drain, and closes idle keep-alive connections (issue #646)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #646. http.ready_path is answered by the gateway process itself:
 * 200 "ready" while it serves, 503 "draining" once a stop or a reload has
 * started. A probe on a NEW connection during that window is checked by
 * fpmng-http-gateway-ready-drain.phpt. This test covers the keep-alive side.
 * The drain closes an idle keep-alive connection at its first tick, so that
 * connection gets no 503 at all. The 503 is seen only by a probe pipelined
 * behind a request that is still in flight on the same connection: the slow
 * request keeps that connection busy, the drain waits for it, and the probe
 * is parsed after the slow reply. */

const SLOW = 3;

$root = sys_get_temp_dir() . '/fpmng-gw-ready-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/app.php", <<<'PHP'
<?php
$n = (int) ($_GET['sleep'] ?? 0);
// A busy wait, not sleep(): the reload signals this child, and sleep() returns at once on a signal.
$end = microtime(true) + $n;
while (microtime(true) < $end) {
    usleep(10000);
}
echo 'app';
PHP);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
process_control_timeout = 10
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.ready_path = /ready
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 2
EOT;

/* The gateway process whose parent is $master, via the setproctitle line
 * fpm_http_gateway_run() installs. 0 when none exists. */
function gateway_pid(int $master): int
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

function read_all($fp, int $seconds): string
{
    stream_set_timeout($fp, $seconds);
    $out = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 65536);
        if ($chunk === false) {
            break;
        }
        if ($chunk === '') {
            $meta = stream_get_meta_data($fp);
            if ($meta['timed_out'] || feof($fp)) {
                break;
            }
            continue;
        }
        $out .= $chunk;
    }
    return $out;
}

/* Reads one reply on a connection that stays open: until the body $body has
 * arrived, or until the read times out. */
function read_until($fp, string $body, int $seconds): string
{
    stream_set_timeout($fp, $seconds);
    $out = '';
    while (!str_ends_with($out, $body)) {
        $chunk = fread($fp, 8192);
        if ($chunk === false || $chunk === '') {
            break;
        }
        $out .= $chunk;
    }
    return $out;
}

/* One request on a fresh connection, closed by the client after the reply. */
function get_once(string $http, string $path): string
{
    $fp = @stream_socket_client("tcp://$http", $errno, $error, 5);
    if (!$fp) {
        throw new RuntimeException("connect: $error");
    }
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");
    $r = read_all($fp, 5);
    fclose($fp);
    return $r;
}

$tester = new FPM\Tester($cfg, '<?php');
try {
    /* forceStderr = false so the gateway's own drain lines land in the
     * error_log file this test reads; -O would send them to the pipe. */
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $master = $tester->getPid();
    $http = $tester->getAddr('ipv4', '[http]');

    $oldGw = gateway_pid($master);
    if ($oldGw === 0) {
        throw new RuntimeException('the gateway process was not found');
    }

    $r = get_once($http, '/ready');
    if (!str_starts_with($r, 'HTTP/1.1 200') || !str_ends_with($r, 'ready')) {
        throw new RuntimeException("the probe is not 200 ready while serving\n$r");
    }
    echo "ready while serving: ok\n";

    /* An idle keep-alive connection: one probe answered, then nothing. The
     * drain closes it at its first tick, so after SIGQUIT the client reads
     * only the close, with no bytes. */
    $idle = @stream_socket_client("tcp://$http", $errno, $error, 5);
    if (!$idle) {
        throw new RuntimeException("connect: $error");
    }
    fwrite($idle, "GET /ready HTTP/1.1\r\nHost: test\r\n\r\n");
    $r = read_until($idle, 'ready', 5);
    if (!str_starts_with($r, 'HTTP/1.1 200') || !str_ends_with($r, 'ready')) {
        throw new RuntimeException("the idle keep-alive connection is not 200 ready\n$r");
    }

    /* The slow request and the probe share one keep-alive connection. The
     * gateway gets SIGQUIT, the drain signal, while the slow request is in
     * flight. The drain keeps that connection open, and the probe is parsed
     * after the slow reply, when the drain is already on. A reload is not
     * used: its master waits for the pool children before it sends SIGQUIT
     * to the gateway, so the probe would be answered first. */
    $ka = @stream_socket_client("tcp://$http", $errno, $error, 5);
    if (!$ka) {
        throw new RuntimeException("connect: $error");
    }
    fwrite($ka, "GET /app.php?sleep=" . SLOW . " HTTP/1.1\r\nHost: test\r\n\r\n"
        . "GET /ready HTTP/1.1\r\nHost: test\r\n\r\n");
    usleep(500000);
    $tester->signal('QUIT', $oldGw);

    $idleTail = read_all($idle, 5);
    $idleClosed = feof($idle);
    fclose($idle);
    if ($idleTail !== '' || !$idleClosed) {
        throw new RuntimeException("the idle keep-alive connection was not closed by the drain\n$idleTail");
    }
    echo "idle keep-alive connection closed by the drain: ok\n";

    $response = read_all($ka, 15);
    fclose($ka);

    $second = strpos($response, 'HTTP/1.1 503');
    if (!str_starts_with($response, 'HTTP/1.1 200') || $second === false
            || !str_contains(substr($response, $second), 'draining')) {
        throw new RuntimeException("the probe behind a slow request is not 503 draining during the drain\n$response");
    }
    echo "503 for a probe pipelined behind a slow request during the drain: ok\n";

    /* The drain ends once the probe connection is closed; the master then
     * replaces the gateway with a new process. */
    $deadline = microtime(true) + 30;
    $newGw = 0;
    while (microtime(true) < $deadline) {
        $pid = gateway_pid($master);
        if ($pid !== 0 && $pid !== $oldGw) {
            $newGw = $pid;
            break;
        }
        usleep(50000);
    }
    if ($newGw === 0) {
        throw new RuntimeException('the old gateway was never replaced');
    }

    $r = get_once($http, '/ready');
    if (!str_starts_with($r, 'HTTP/1.1 200') || !str_ends_with($r, 'ready')) {
        throw new RuntimeException("the new generation is not 200 ready\n$r");
    }
    echo "new generation is ready: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/app.php");
    @rmdir($root);
}
?>
--EXPECT--
ready while serving: ok
idle keep-alive connection closed by the drain: ok
503 for a probe pipelined behind a slow request during the drain: ok
new generation is ready: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
