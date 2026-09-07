--TEST--
FPM http gateway: a full pool answers 503 + Retry-After, not 502 (task 031)
--SKIPIF--
<?php
include "skipif.inc";
?>
--FILE--
<?php

require_once "tester.inc";

// Task 031, acceptance criterion 3: a full pool is distinguishable from any
// other upstream failure in the HTTP response. With pm.max_children = 1 and
// one gateway, the first (slow) request pins the only upstream connection
// and its worker; the second request finds no idle upstream and no budget
// for a new one, so the gateway answers 503 + Retry-After immediately
// instead of queueing towards an eventual 502.

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
EOT;

$tester = new FPM\Tester($config, '<?php sleep(5); echo "slow";');
$tester->start();
$tester->expectLogStartNotices();

$script = '/' . basename($tester->makeSourceFile());

$httpAddr = $tester->getAddr('ipv4', '[http]');
[$host, $port] = explode(':', $httpAddr);

// Request 1 occupies the only worker for five seconds. Non-blocking: we only
// need it to be in flight while request 2 arrives.
$fp1 = fsockopen($host, (int) $port, $errno, $errstr, 5);
if (!$fp1) {
    echo "FAIL: connect #1: $errstr ($errno)\n";
    exit(1);
}
fwrite($fp1, "GET $script HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
stream_set_blocking($fp1, false);

// Give the gateway a moment to dispatch request 1 to the worker before
// request 2 arrives; without this the two could race at accept() time and
// the test would be flaky.
usleep(500000);

$start = microtime(true);
$fp2 = fsockopen($host, (int) $port, $errno, $errstr, 5);
if (!$fp2) {
    echo "FAIL: connect #2: $errstr ($errno)\n";
    exit(1);
}
fwrite($fp2, "GET $script HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
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

if (!str_starts_with($response, 'HTTP/1.1 503') && !str_starts_with($response, 'HTTP/1.0 503')) {
    echo "FAIL: expected a 503 on a full pool, got:\n$response\n";
    exit(1);
}
if (!preg_match('/^Retry-After:\s*\d+/mi', $response)) {
    echo "FAIL: 503 came without a Retry-After header:\n$response\n";
    exit(1);
}
// The whole point: the 503 is immediate, not after waiting out the slow
// request. Allow generous slack for a loaded shared CI box.
if ($elapsed > 3) {
    echo sprintf("FAIL: 503 was not immediate (%.1f s); the request queued\n", $elapsed);
    exit(1);
}

// Request 1 still completes normally -- the 503 path must not disturb the
// worker that is in flight.
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

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
