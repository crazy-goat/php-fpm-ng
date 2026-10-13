--TEST--
fpm-ng: an upload still arriving when the gateway drains is cut, not drained (issue #641 phase-1 limitation)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #641 review, point 1. The drain finishes a request that has already
 * been dispatched to a target worker. It does NOT finish a request whose body
 * is still being uploaded: evhttp buffers a whole body before dispatching it,
 * and by the time the upload dispatches the master has already stopped the
 * target pool's workers (fpm_pctl_action_next() signals every child;
 * fpm_pctl_exec() -> fpm_http_cleanup() drains the gateways afterwards), so
 * there is no worker left to answer. The connection is cut at once, not held
 * for the deadline.
 *
 * This test PINS that limitation so it cannot be mistaken for a drain later. It
 * is expected to be replaced when the gateway is drained before the target
 * workers are stopped (phase 2 / follow-up). The gateway is replaced well
 * before process_control_timeout, and the uploading client gets no response. */

const BODY = 4 * 1024 * 1024;
const FIRST = 64 * 1024;
const PCT = 4;
$pct = PCT;

$root = sys_get_temp_dir() . '/fpmng-gw-drain-upload-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/upload.php", <<<'PHP'
<?php
$in = file_get_contents('php://input');
header('Content-Type: text/plain');
echo strlen($in);
PHP);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
log_level = notice
process_control_timeout = $pct
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.read_timeout = 0
http.keepalive_timeout = 0
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

$tester = new FPM\Tester($cfg, '<?php');
try {
    $tester->start([], false);
    $tester->switchLogSource('{{FILE:LOG}}');
    $tester->expectLogStartNotices();
    $master = $tester->getPid();
    $http = $tester->getAddr('ipv4', '[http]');

    $oldGw = gateway_pid($master);
    if ($oldGw === 0) {
        throw new RuntimeException('the gateway process was not found');
    }

    $sock = stream_socket_client("tcp://$http", $errno, $error, 5);
    if (!$sock) {
        throw new RuntimeException("connect: $error");
    }
    $head = "POST /upload.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n"
        . 'Content-Length: ' . BODY . "\r\n\r\n";
    fwrite($sock, $head . str_repeat('A', FIRST));
    /* Let the gateway buffer the partial body. */
    usleep(500000);

    $t0 = microtime(true);
    $tester->signal('USR2');

    /* The old gateway is replaced only after the master has re-exec'd; that
     * happens at once here because the drain sees nothing dispatched (the
     * upload never reached a worker). */
    $deadline = microtime(true) + 20;
    $newGw = 0;
    while (microtime(true) < $deadline) {
        $pid = gateway_pid($master);
        if ($pid !== 0 && $pid !== $oldGw) {
            $newGw = $pid;
            break;
        }
        usleep(20000);
    }
    $elapsed = microtime(true) - $t0;
    if ($newGw === 0) {
        throw new RuntimeException('the old gateway was never replaced');
    }
    /* The limitation: cut at once, not held for process_control_timeout. */
    if ($elapsed > PCT - 1.0) {
        throw new RuntimeException(sprintf(
            'the gateway held the upload for %.2fs (deadline %ds): uploads may now drain -- update this test and docs/shutdown-timeouts.md',
            $elapsed, PCT));
    }
    echo "upload cut at once, not drained: ok\n";

    /* The uploading client got no response: the connection was cut. */
    $response = read_all($sock, 5);
    fclose($sock);
    if (str_contains($response, 'HTTP/1.1 200')) {
        throw new RuntimeException("the upload was answered with 200; it drained, update this test\n$response");
    }
    echo "uploading client got no 200: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/upload.php");
    @rmdir($root);
}
?>
--EXPECT--
upload cut at once, not drained: ok
uploading client got no 200: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
