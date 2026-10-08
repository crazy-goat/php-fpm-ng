--TEST--
fpm-ng: http.ready_path answers 200 while the gateway serves and 503 on a keep-alive connection during a drain (issue #646)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #646, phase 1. http.ready_path is answered by the gateway process
 * itself: 200 "ready" while it serves, 503 "draining" once SIGQUIT has set
 * gw->stopping. The drain also removes the listeners, so a new connection is
 * not answered during the drain at all. Only a client that already holds a
 * keep-alive connection can see the 503, which is why this test uses one: a
 * slow request keeps the connection busy, the drain waits for it, and the
 * probe is pipelined behind it on the same connection. #661 refines this. */

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
    $response = read_all($ka, 15);
    fclose($ka);

    $second = strpos($response, 'HTTP/1.1 503');
    if (!str_starts_with($response, 'HTTP/1.1 200') || $second === false
            || !str_contains(substr($response, $second), 'draining')) {
        throw new RuntimeException("the probe behind a slow request is not 503 draining during the drain\n$response");
    }
    echo "503 while draining on a keep-alive connection: ok\n";

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
503 while draining on a keep-alive connection: ok
new generation is ready: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
