--TEST--
FPM http gateway: pool_full_policy = wait serves a burst of pm.max_children parallel requests with two gateway processes (issue #735)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

// Issue #735. Each gateway process keeps its own idle upstream connections, and
// the budget they pin is shared. After a burst the two processes hold, say, 4
// idle connections each; when the next burst of 8 lands 6 + 2 on them, the
// process with 3 needs a fourth connection, finds the budget full and -- before
// the fix -- waited http.pool_full_wait_ms for a release that only the other
// process could make, then answered 503 with 8 idle workers (3 of 10 runs in
// the #602 review). A burst of N <= pm.max_children parallel requests must be
// answered 200 in every round.

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
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR[fastcgi]}}
pm = static
pm.max_children = 8
chdir = $docroot
EOT;

$tester = new FPM\Tester($config, '<?php usleep(150000); echo "ok";');
$tester->start();
$tester->expectLogStartNotices();

$script = '/' . basename($tester->makeSourceFile());

$httpAddr = $tester->getAddr('ipv4', '[http]');
[$host, $port] = explode(':', $httpAddr);

$bad = [];
for ($round = 0; $round < 12; $round++) {
    $start = microtime(true);
    $fps = [];
    for ($i = 0; $i < 8; $i++) {
        $fp = fsockopen($host, (int) $port, $errno, $errstr, 5);
        if (!$fp) {
            echo "FAIL: connect round $round #$i: $errstr ($errno)\n";
            exit(1);
        }
        fwrite($fp, "GET $script HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
        $fps[] = $fp;
    }
    foreach ($fps as $i => $fp) {
        $response = stream_get_contents($fp);
        fclose($fp);
        if (!str_contains($response, ' 200 ') || !str_contains((string) strstr($response, "\r\n\r\n"), 'ok')) {
            $bad[] = "round $round #$i: " . strtok($response, "\r\n");
        }
    }
    // A request that had to wait out a reclaim finishes in a fraction of a
    // second; one that waited http.pool_full_wait_ms would take 4 s.
    $elapsed = microtime(true) - $start;
    if ($elapsed > 3) {
        $bad[] = sprintf("round %d took %.1f s", $round, $elapsed);
    }
    usleep(50000);
}
if ($bad) {
    echo "FAIL:\n" . implode("\n", $bad) . "\n";
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
