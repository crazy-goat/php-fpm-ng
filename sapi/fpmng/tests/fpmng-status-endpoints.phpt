--TEST--
fpm-ng: pool.type = status answers /status and /metrics (docs/NOTES.md §3j/§3u)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

function httpLine(string $addr, string $path): string
{
    $fp = @stream_socket_client("tcp://$addr", $errno, $errstr, 5);
    if (!$fp) {
        return '';
    }
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    $response = stream_get_contents($fp);
    fclose($fp);
    return $response === false ? '' : $response;
}

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[monitor]
listen = {{ADDR}}
pool.type = status
EOT;

$tester = new FPM\Tester($cfg, '<?php /* unused */');
$tester->start();
$tester->expectLogStartNotices();
$addr = $tester->getAddr('ipv4');

$status = httpLine($addr, '/status');
if (!str_contains($status, 'HTTP/1.1 200') || !str_contains($status, 'application/json')) {
    echo "FAIL: /status response head=" . substr($status, 0, 120) . "\n";
    exit(1);
}
if (!str_contains($status, '"pools"') && !str_contains($status, 'monitor')) {
    echo "FAIL: /status body missing pool data\n";
    exit(1);
}
echo "/status: ok\n";

$metrics = httpLine($addr, '/metrics');
if (!str_contains($metrics, 'HTTP/1.1 200')) {
    echo "FAIL: /metrics response head=" . substr($metrics, 0, 120) . "\n";
    exit(1);
}
if (!str_contains($metrics, 'fpmng_') && !str_contains($metrics, '# HELP')) {
    echo "FAIL: /metrics body missing prometheus text\n";
    exit(1);
}
echo "/metrics: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
/status: ok
/metrics: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
