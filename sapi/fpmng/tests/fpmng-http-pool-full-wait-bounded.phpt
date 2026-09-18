--TEST--
FPM http gateway: pool_full_policy = wait still bounds queue depth and wait time on a misconfigured/CPU-bound pool (issue #309)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php

require_once "tester.inc";

// Issue #309's regression requirement: a pool that opts into the wait policy
// but is actually CPU-bound (or otherwise misconfigured with a worker that
// stays busy far longer than http.pool_full_wait_ms) must not be made
// silently worse -- it must still fail closed, bounded, rather than hanging
// or queueing without limit. Two bounds are exercised here with
// pm.max_children = 1 and a single slow (CPU-bound-style) worker:
//
//   1. http.pool_full_queue_max: once the queue itself is full, a further
//      request is rejected immediately (503), not added to the queue.
//   2. http.pool_full_wait_ms: a request that does get queued, but whose
//      wait outlives the bound because the busy worker does not free up in
//      time, is rejected (503) rather than left waiting indefinitely.

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
http.pool_full_policy = wait
http.pool_full_queue_max = 1
http.pool_full_wait_ms = 300
EOT;

// The one worker sleeps far longer than the wait bound (300ms), standing in
// for CPU-bound work that cannot drain the queue any faster no matter how long
// a request waits. sleep(2), not 5 (issue #399): every deadline this test
// depends on falls inside the first 700ms -- request 2 is queued at 300ms and
// its 300ms bound expires around 600ms, request 3 is rejected at 400ms -- so
// 2 s is still more than three times the longest of them, and the test no
// longer spends three seconds reading out request 1 at the end. Both the
// $elapsed3 > 1 and $elapsed2 > 2 bounds are unaffected.
$tester = new FPM\Tester($config, '<?php sleep(2); echo "slow";');
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

function connectAndSend(string $host, int $port, string $script): mixed
{
    $fp = fsockopen($host, $port, $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    fwrite($fp, "GET $script HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
    return $fp;
}

// Request 1 pins the only worker for 5s.
$fp1 = connectAndSend($host, (int) $port, $script);
stream_set_blocking($fp1, false);
usleep(300000);

// Request 2 arrives while the worker is busy: queue is empty (cap 1), so
// this one is admitted onto gw->waiting.
$fp2 = connectAndSend($host, (int) $port, $script);
stream_set_blocking($fp2, false);
usleep(100000);

// Request 3 arrives while the queue is already at its cap of 1: this one
// must be rejected immediately, not appended past the cap.
$start3 = microtime(true);
$fp3 = connectAndSend($host, (int) $port, $script);
stream_set_blocking($fp3, true);
$resp3 = readResponse($fp3);
fclose($fp3);
$elapsed3 = microtime(true) - $start3;

if (!str_starts_with($resp3, 'HTTP/1.1 503') && !str_starts_with($resp3, 'HTTP/1.0 503')) {
    echo "FAIL: expected 503 once the queue was at pool_full_queue_max, got:\n$resp3\n";
    exit(1);
}
if ($elapsed3 > 1) {
    echo sprintf("FAIL: over-cap request was not rejected immediately (%.2fs) -- unbounded queueing\n", $elapsed3);
    exit(1);
}
echo "over-cap request rejected immediately: ok\n";

// Request 2, still on the queue, must be rejected once its wait bound
// (300ms) expires -- the busy worker will not free up for another ~4.5s, so
// this proves the wait bound fires rather than the request hanging until
// the worker is eventually free.
stream_set_blocking($fp2, true);
$start2 = microtime(true);
$resp2 = readResponse($fp2);
fclose($fp2);
$elapsed2 = microtime(true) - $start2;

if (!str_starts_with($resp2, 'HTTP/1.1 503') && !str_starts_with($resp2, 'HTTP/1.0 503')) {
    echo "FAIL: expected the queued request to be rejected once pool_full_wait_ms expired, got:\n$resp2\n";
    exit(1);
}
// It was already in flight for ~0.4s (0.3s before request 3, 0.1s more) when
// this read started, and the bound is 300ms, so the remaining wait here
// should be well under a second -- generous slack for a loaded shared CI box,
// but nowhere near the 5s the worker actually needs.
if ($elapsed2 > 2) {
    echo sprintf("FAIL: queued request was not bounded by pool_full_wait_ms (%.2fs)\n", $elapsed2);
    exit(1);
}
echo "expired queued request rejected within its wait bound: ok\n";

// Request 1 still completes normally: the wait policy's rejections must not
// disturb the in-flight worker.
stream_set_blocking($fp1, true);
$resp1 = readResponse($fp1);
fclose($fp1);
if (!str_contains($resp1, ' 200 ') || !str_contains($resp1, 'slow')) {
    echo "FAIL: the in-flight request did not complete:\n$resp1\n";
    exit(1);
}
echo "in-flight request unaffected: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
over-cap request rejected immediately: ok
expired queued request rejected within its wait bound: ok
in-flight request unaffected: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
