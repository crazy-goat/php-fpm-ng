--TEST--
FPM http gateway: two gateway processes do not starve each other of workers (issue #735)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

// Issue #735. The gateway processes share one admission budget but each keeps
// its own persistent connections, and an idle one pins a worker. With
// pm.max_children = 1 and http.gateways = 2, the process that served request N
// kept the only worker for up to http.idle_timeout, so request N+1 -- when the
// kernel gave it to the OTHER process -- found no budget and was answered 503
// although nothing was in flight (18 of 25 runs of 8 back-to-back requests in
// #728). Default pool_full_policy, sequential requests: every one must be 200.

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
for ($i = 0; $i < 40; $i++) {
    $fp = fsockopen($host, (int) $port, $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect #$i: $errstr ($errno)\n";
        exit(1);
    }
    fwrite($fp, "GET $script HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
    $response = stream_get_contents($fp);
    fclose($fp);
    if (!str_contains($response, ' 200 ') || !str_contains((string) strstr($response, "\r\n\r\n"), 'ok')) {
        $bad[] = "#$i: " . strtok($response, "\r\n");
    }
}
if ($bad) {
    echo "FAIL: " . count($bad) . " of 40 sequential requests were not answered 200:\n" . implode("\n", $bad) . "\n";
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
