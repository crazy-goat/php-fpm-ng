--TEST--
fpm-ng: a full gateway target's 503s are counted per target in the gateway's shared counter (issue #390)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #390, acceptance criterion 2. One gateway, one route to a single-worker
 * "events" pool, and a "web" target that never goes full. Rejections are the
 * per-target rejected_total (#341's series, now read from the segment), and a
 * 503 storm on events must not move web's counter. */

$docroot = sys_get_temp_dir() . '/fpmng-gw-503-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php',
    '<?php if (str_contains($_SERVER["REQUEST_URI"] ?? "", "slow")) { sleep(2); } echo "ok";');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.front_controller = /index.php
http.route[events] = /sse
operator.metrics_listen = {{ADDR[operator]}}
http.route[web] = /
[events]
pool.type = fastcgi
listen = {{ADDR[events]}}
pm = static
pm.max_children = 1
[web]
pool.type = fastcgi
listen = {{ADDR[web]}}
chdir = $docroot
pm = static
pm.max_children = 2
EOT;

function request(string $host, int $port, string $path)
{
    $fp = fsockopen($host, $port, $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect for $path: $errstr ($errno)\n";
        exit(1);
    }
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
    return $fp;
}

function readAll($fp): string
{
    stream_set_blocking($fp, true);
    $out = '';
    while (!feof($fp)) {
        $chunk = fgets($fp);
        if ($chunk === false) {
            break;
        }
        $out .= $chunk;
    }
    fclose($fp);
    return $out;
}

function rejected(string $metrics, string $target): ?int
{
    return preg_match('/fpmng_gateway_rejected_total\{pool="gw",target="' . preg_quote($target, '/') . '"\} (\d+)/',
        $metrics, $m) ? (int) $m[1] : null;
}

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $httpAddr = $tester->getAddr('ipv4', '[http]');
    [$host, $port] = explode(':', $httpAddr);
    $operator = $tester->getListen('{{ADDR[operator]}}');

    $before = fpmng_operator_body($operator, '/metrics');
    $eventsBefore = rejected($before, 'events');
    $webBefore = rejected($before, 'web');
    if ($eventsBefore !== 0 || $webBefore !== 0) {
        echo "FAIL: rejected_total not zero before any rejection: events=" . var_export($eventsBefore, true) .
            " web=" . var_export($webBefore, true) . "\n$before\n";
        exit(1);
    }
    echo "before: both zero\n";

    // Pin the single events worker, then send one more so it finds no budget.
    $slow = request($host, (int) $port, '/sse/slow');
    stream_set_blocking($slow, false);
    usleep(500000);

    $rejectedBody = readAll(request($host, (int) $port, '/sse/second'));
    if (!str_starts_with($rejectedBody, 'HTTP/1.1 503') && !str_starts_with($rejectedBody, 'HTTP/1.0 503')) {
        echo "FAIL: expected 503 on the full events target, got:\n$rejectedBody\n";
        exit(1);
    }
    echo "sse-second: 503\n";

    $after = fpmng_operator_body($operator, '/metrics');
    $eventsAfter = rejected($after, 'events');
    $webAfter = rejected($after, 'web');
    if ($eventsAfter !== $eventsBefore + 1) {
        echo "FAIL: rejected_total{target=events} should rise by 1: before=" . var_export($eventsBefore, true) .
            " after=" . var_export($eventsAfter, true) . "\n$after\n";
        exit(1);
    }
    if ($webAfter !== 0) {
        echo "FAIL: rejected_total{target=web} moved when only events was full: " . var_export($webAfter, true) . "\n$after\n";
        exit(1);
    }
    echo "after: events +1, web still zero\n";

    $first = readAll($slow);
    if (!str_contains($first, ' 200 ')) {
        echo "FAIL: the in-flight slow request did not complete:\n$first\n";
        exit(1);
    }
    echo "slow-request-completed: ok\n";

    echo "Done\n";
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink($docroot . '/index.php');
    @rmdir($docroot);
}
?>
--EXPECT--
before: both zero
sse-second: 503
after: events +1, web still zero
slow-request-completed: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
