--TEST--
FPM http gateway: with every upstream busy the reclaim policy still answers 503 at once (issue #156, THROWAWAY)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php

require_once "tester.inc";

/* THROWAWAY test, second criterion of issue #156: the reclaim path must not
 * turn into unbounded waiting. Both workers are held by slow requests here,
 * so every upstream is BUSY rather than idle, no gateway advertises anything
 * to reclaim, and the answer has to be the unmodified 503 + Retry-After with
 * no grace period spent at all.
 *
 * Same shape as fpmng-http-pool-full-503.phpt, which this branch also leaves
 * passing unchanged -- that one has http.gateways = 1 and is the "status quo
 * is untouched" criterion. */

$docroot = __DIR__;

putenv('FPMNG_GW_RECLAIM=1');
putenv('FPMNG_GW_RECLAIM_MS=50');

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
[busy]
listen = {{ADDR[fastcgi]}}
pool.type = http
pm = static
pm.max_children = 2
chdir = $docroot
http.gateways = 2
http.reuseport = 1
http.listen = {{ADDR[http]}}
EOT;

$tester = new FPM\Tester($config, '<?php sleep(5); echo "slow";');
$tester->start();
$tester->expectLogStartNotices();

$script = '/' . basename($tester->makeSourceFile());

$httpAddr = $tester->getAddr('ipv4', '[http]');
[$host, $port] = explode(':', $httpAddr);
$port = (int) $port;

$slow = [];
for ($i = 0; $i < 2; $i++) {
    $fp = fsockopen($host, $port, $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect #$i: $errstr ($errno)\n";
        exit(1);
    }
    fwrite($fp, "GET $script HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
    stream_set_blocking($fp, false);
    $slow[] = $fp;
}

// Both requests have to be dispatched before the third arrives, otherwise the
// two could still be racing at accept() time.
usleep(500000);

$start = microtime(true);
$fp = fsockopen($host, $port, $errno, $errstr, 5);
if (!$fp) {
    echo "FAIL: connect #3: $errstr ($errno)\n";
    exit(1);
}
fwrite($fp, "GET $script HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
$response = stream_get_contents($fp);
fclose($fp);
$elapsed = microtime(true) - $start;

if (!str_contains($response, ' 503 ')) {
    echo "FAIL: expected a 503 with every upstream busy, got:\n$response\n";
    exit(1);
}
if (!preg_match('/^Retry-After:\s*\d+/mi', $response)) {
    echo "FAIL: 503 came without a Retry-After header:\n$response\n";
    exit(1);
}
// Generous for a loaded shared box, but far below the 5 s the slow requests
// hold their workers for: the point is that nothing queued.
if ($elapsed > 3) {
    echo sprintf("FAIL: the 503 was not immediate (%.1f s)\n", $elapsed);
    exit(1);
}

foreach ($slow as $i => $fp) {
    stream_set_blocking($fp, true);
    $body = stream_get_contents($fp);
    fclose($fp);
    if (!str_contains($body, 'slow')) {
        echo "FAIL: in-flight request $i did not complete:\n$body\n";
        exit(1);
    }
}

// Nothing could be reclaimed, so nothing was: no sibling was even rung.
$tester->expectNoLogPattern('/released an idle upstream on a sibling/');

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
