--TEST--
fpm-ng: http gateway answers ping.path immediately even when the backend pool is saturated (issue #382)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php

require_once "tester.inc";

/* Before #382, /ping was routed like any other request: with pm.max_children
 * = 1 and the only worker pinned by an in-flight request, /ping waited under
 * http.pool_full_policy same as anything else -- a saturated backend failed
 * its own liveness probe even though the gateway process itself was healthy.
 * This pins the fix: ping.path is answered by the gateway before it ever
 * looks at the pool's budget, so it is immediate regardless of saturation. */

$docroot = __DIR__;

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
[full]
listen = {{ADDR[fastcgi]}}
pool.type = http
pm = static
pm.max_children = 1
chdir = $docroot
http.gateways = 1
http.listen = {{ADDR[http]}}
ping.path = /ping
ping.response = pong
EOT;

$tester = new FPM\Tester($config, '<?php sleep(5); echo "slow";');
$tester->start();
$tester->expectLogStartNotices();

$script = '/' . basename($tester->makeSourceFile());

$httpAddr = $tester->getAddr('ipv4', '[http]');
[$host, $port] = explode(':', $httpAddr);

// Occupy the only worker for five seconds.
$fp1 = fsockopen($host, (int) $port, $errno, $errstr, 5);
if (!$fp1) {
    echo "FAIL: connect #1: $errstr ($errno)\n";
    exit(1);
}
fwrite($fp1, "GET $script HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
stream_set_blocking($fp1, false);

// Give the gateway a moment to dispatch request 1 before the ping arrives.
usleep(500000);

$start = microtime(true);
$fp2 = fsockopen($host, (int) $port, $errno, $errstr, 5);
if (!$fp2) {
    echo "FAIL: connect #2: $errstr ($errno)\n";
    exit(1);
}
fwrite($fp2, "GET /ping HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
$response = '';
while (!feof($fp2)) {
    $chunk = fgets($fp2);
    if ($chunk === false) {
        break;
    }
    $response .= $chunk;
}
fclose($fp2);
$elapsed = microtime(true) - $start;

if (!str_starts_with($response, 'HTTP/1.1 200') && !str_starts_with($response, 'HTTP/1.0 200')) {
    echo "FAIL: expected an immediate 200 pong on a full pool, got:\n$response\n";
    exit(1);
}
if (!str_contains($response, 'pong')) {
    echo "FAIL: ping response did not contain pong:\n$response\n";
    exit(1);
}
if ($elapsed > 3) {
    echo sprintf("FAIL: ping was not immediate (%.1f s); it waited on the saturated pool\n", $elapsed);
    exit(1);
}
echo "ping-immediate-when-saturated: ok\n";

// Request 1 still completes normally.
stream_set_blocking($fp1, true);
$first = '';
while (!feof($fp1)) {
    $chunk = fgets($fp1);
    if ($chunk === false) {
        break;
    }
    $first .= $chunk;
}
fclose($fp1);
if (!str_contains($first, ' 200 ') || !str_contains($first, 'slow')) {
    echo "FAIL: the in-flight request did not complete:\n$first\n";
    exit(1);
}
echo "in-flight-request-completed: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
ping-immediate-when-saturated: ok
in-flight-request-completed: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
