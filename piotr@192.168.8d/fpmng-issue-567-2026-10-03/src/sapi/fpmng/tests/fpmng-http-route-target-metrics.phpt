--TEST--
fpm-ng: a gateway's metrics carry rejected_total per target, not blended together (issue #341)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #341, requirement 3: fpmng_gateway_rejected_total{pool=,target=} has
 * to isolate one full target from the others on the SAME gateway -- otherwise
 * a 503 storm against /sse/* looks, on the dashboard, exactly like one
 * against /. One gateway ("web"), one route to a single-worker "events" pool;
 * "web" itself is the gateway's own (unrouted) target -- fpm_http_routes_build()
 * puts it in targets[0] whenever "/" is not claimed by a route -- and it never
 * goes full in this test. */

$docroot = sys_get_temp_dir() . '/fpmng-http-route-metrics-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php',
    '<?php if (str_contains($_SERVER["REQUEST_URI"] ?? "", "slow")) { sleep(2); } echo "ok";');

$config = <<<EOT
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
operator.metrics_path = /metrics
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 2

[events]
listen = {{ADDR[events]}}
pm = static
pm.max_children = 1
EOT;

$tester = new FPM\Tester($config, '<?php echo "ok";');
$tester->start();
$tester->expectLogStartNotices();

$httpAddr = $tester->getAddr('ipv4', '[http]');
[$host, $port] = explode(':', $httpAddr);
$operator = $tester->getListen('{{ADDR[operator]}}');

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

function rejectedTotal(string $metrics, string $target): ?int
{
    /* Issue #388: the gateway pool is [gw] now, and the old implicit own-pool
     * target is the explicit [web] fastcgi target routed at "/". */
    if (preg_match('/fpmng_gateway_rejected_total\{pool="gw",target="' . preg_quote($target, '/') . '"\} (\d+)/', $metrics, $m)) {
        return (int) $m[1];
    }
    return null;
}

$before = fpmng_operator_body($operator, '/metrics');
$eventsBefore = rejectedTotal($before, 'events');
$webBefore = rejectedTotal($before, 'web');
if ($eventsBefore === null || $webBefore === null) {
    echo "FAIL: rejected_total missing for events or web before:\n$before\n";
    exit(1);
}
if ($eventsBefore !== 0 || $webBefore !== 0) {
    echo "FAIL: rejected_total not zero before any rejection: events=$eventsBefore web=$webBefore\n";
    exit(1);
}
echo "before: both zero\n";

// Pin the single events worker.
$slow = request($host, (int) $port, '/sse/slow');
stream_set_blocking($slow, false);
usleep(500000);

// This one finds no budget on the events target and is rejected.
$rejected = readAll(request($host, (int) $port, '/sse/second'));
if (!str_starts_with($rejected, 'HTTP/1.1 503') && !str_starts_with($rejected, 'HTTP/1.0 503')) {
    echo "FAIL: expected 503 on the full events target, got:\n$rejected\n";
    exit(1);
}
echo "sse-second: 503\n";

// The gateway's own target keeps serving without ever going full.
$body = readAll(request($host, (int) $port, '/'));
if (!str_contains($body, ' 200 ')) {
    echo "FAIL: / stopped being served while events was full:\n$body\n";
    exit(1);
}
echo "root: 200\n";

$after = fpmng_operator_body($operator, '/metrics');
$eventsAfter = rejectedTotal($after, 'events');
$webAfter = rejectedTotal($after, 'web');
if ($eventsAfter === null || $webAfter === null) {
    echo "FAIL: rejected_total missing for events or web after:\n$after\n";
    exit(1);
}

if ($eventsAfter <= $eventsBefore) {
    echo "FAIL: rejected_total{target=events} did not rise: before=$eventsBefore after=$eventsAfter\n";
    exit(1);
}
echo "after: events rose\n";

if ($webAfter !== 0) {
    echo "FAIL: rejected_total{target=web} moved when only events was full: $webAfter\n";
    exit(1);
}
echo "after: web still zero\n";

$first = readAll($slow);
if (!str_contains($first, ' 200 ')) {
    echo "FAIL: the in-flight slow request did not complete:\n$first\n";
    exit(1);
}
echo "slow-request-completed: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink($docroot . '/index.php');
@rmdir($docroot);

?>
Done
--EXPECT--
before: both zero
sse-second: 503
root: 200
after: events rose
after: web still zero
slow-request-completed: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
