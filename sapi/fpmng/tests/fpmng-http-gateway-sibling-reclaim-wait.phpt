--TEST--
FPM http gateway: pool_full_policy = wait is not stalled by an idle connection of the other gateway process (issue #735)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

// Issue #735. With pm.max_children = 1 and http.gateways = 2, the process that
// served a request keeps the only worker on an idle upstream connection. The
// next request, when it lands on the other process, finds the shared budget
// full and is queued; before the fix nothing woke that queue until
// http.pool_full_wait_ms ran out, and the answer was 503 with an idle worker.
// Every sequential request must be 200, and none may wait anywhere near the
// bound.

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
http.gateways = 2
http.pool_full_policy = wait
http.pool_full_queue_max = 16
http.pool_full_wait_ms = 4000
http.route[small] = /
[small]
pool.type = fastcgi
listen = {{ADDR[fastcgi]}}
pm = static
pm.max_children = 1
chdir = $docroot
EOT;

$tester = new FPM\Tester($config, '<?php echo "ok";');
$tester->start();
$tester->expectLogStartNotices();

$script = '/' . basename($tester->makeSourceFile());

$httpAddr = $tester->getAddr('ipv4', '[http]');
[$host, $port] = explode(':', $httpAddr);

$bad = [];
for ($i = 0; $i < 12; $i++) {
    $start = microtime(true);
    $fp = fsockopen($host, (int) $port, $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect #$i: $errstr ($errno)\n";
        exit(1);
    }
    fwrite($fp, "GET $script HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
    $response = stream_get_contents($fp);
    fclose($fp);
    $elapsed = microtime(true) - $start;
    if (!str_contains($response, ' 200 ') || !str_contains((string) strstr($response, "\r\n\r\n"), 'ok')) {
        $bad[] = "#$i: " . strtok($response, "\r\n");
    } elseif ($elapsed > 2) {
        $bad[] = sprintf("#%d took %.1f s", $i, $elapsed);
    }
}
if ($bad) {
    echo "FAIL: " . count($bad) . " of 12 sequential requests failed:\n" . implode("\n", $bad) . "\n";
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
