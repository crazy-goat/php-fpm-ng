--TEST--
FPM http gateway: pool_full_policy = wait queues and later dispatches a request instead of rejecting it (issue #309)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

// Issue #309, the IO-light-benefit case: with pm.max_children = 1 and the
// wait policy on, a request that arrives while the only worker is busy is
// held on gw->waiting rather than answered 503 immediately, and is dispatched
// normally once the first request finishes and the worker goes idle again --
// as long as that happens within http.pool_full_wait_ms. This is the
// behavior docs/spike-gateway-poolfull-report.md measured a real win from
// for IO-light workloads.

$docroot = __DIR__;

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
http.pool_full_policy = wait
http.pool_full_queue_max = 8
http.pool_full_wait_ms = 3000
http.route[full] = /
[full]
pool.type = fastcgi
listen = {{ADDR[fastcgi]}}
pm = static
pm.max_children = 1
chdir = $docroot
EOT;

$tester = new FPM\Tester($config, '<?php if (isset($_GET["slow"])) { usleep(800000); } echo "done:" . getmypid();');
$tester->start();
$tester->expectLogStartNotices();

$script = '/' . basename($tester->makeSourceFile());

$httpAddr = $tester->getAddr('ipv4', '[http]');
[$host, $port] = explode(':', $httpAddr);

function readResponse($fp): string
{
    $out = '';
    while (!feof($fp)) {
        $chunk = fgets($fp);
        if ($chunk === false) {
            break;
        }
        $out .= $chunk;
    }
    return $out;
}

// Request 1 occupies the only worker for 0.8s.
$fp1 = fsockopen($host, (int) $port, $errno, $errstr, 5);
if (!$fp1) {
    echo "FAIL: connect #1: $errstr ($errno)\n";
    exit(1);
}
fwrite($fp1, "GET $script?slow HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
stream_set_blocking($fp1, false);

// Give the gateway a moment to dispatch request 1 before request 2 arrives;
// without this the two could race at accept() time and the test would be
// flaky (same reasoning as fpmng-http-pool-full-503.phpt).
usleep(300000);

// Request 2 arrives while the pool is full. Under pool_full_policy = wait
// this must queue, not reject -- and it must come back 200 once request 1
// finishes, well within the 3s wait bound.
$start = microtime(true);
$fp2 = fsockopen($host, (int) $port, $errno, $errstr, 5);
if (!$fp2) {
    echo "FAIL: connect #2: $errstr ($errno)\n";
    exit(1);
}
fwrite($fp2, "GET $script HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
stream_set_blocking($fp2, true);
$response = readResponse($fp2);
fclose($fp2);
$elapsed = microtime(true) - $start;

if (!str_starts_with($response, 'HTTP/1.1 200') && !str_starts_with($response, 'HTTP/1.0 200')) {
    echo "FAIL: expected the queued request to eventually succeed, got:\n$response\n";
    exit(1);
}
if (!str_contains($response, 'done:')) {
    echo "FAIL: queued request did not reach the script:\n$response\n";
    exit(1);
}
if (!preg_match('/^X-Fpmng-Queue-Wait:\s*(\d+)/mi', $response, $m)) {
    echo "FAIL: queued request's response carried no X-Fpmng-Queue-Wait header:\n$response\n";
    exit(1);
}
$waited = (int) $m[1];
if ($waited <= 0) {
    echo "FAIL: X-Fpmng-Queue-Wait was not positive despite queueing behind a busy worker: $waited\n";
    exit(1);
}
// It queued behind an 0.8s request, so it should have waited a good chunk of
// that -- but well inside the 3s bound configured above. Generous bounds for
// a loaded shared CI box.
if ($elapsed < 0.3 || $elapsed > 3) {
    echo sprintf("FAIL: queued request took an implausible %.2fs end to end\n", $elapsed);
    exit(1);
}

stream_set_blocking($fp1, true);
$first = readResponse($fp1);
fclose($fp1);
if (!str_contains($first, ' 200 ') || !str_contains($first, 'done:')) {
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
