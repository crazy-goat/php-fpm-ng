--TEST--
FPM http gateway: http.max_body rejects an oversized request with 413 (task 031)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php

require_once "tester.inc";

// Task 031, acceptance criterion 2 (backpressure decision): the gateway
// buffers a whole request body in memory (libevent evhttp does that before
// the request callback runs), so the only bound is a hard cap. The cap was a
// compile-time FPM_HTTP_MAX_BODY of 32 MiB; it is now the http.max_body
// directive so small VPS deployments can lower it instead of forking.

$docroot = __DIR__;

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
[body]
listen = {{ADDR[fastcgi]}}
pool.type = http
pm = static
pm.max_children = 1
chdir = $docroot
http.listen = {{ADDR[http]}}
http.max_body = 1k
EOT;

$tester = new FPM\Tester($config, '<?php echo "len=", $_SERVER["CONTENT_LENGTH"] ?? "none";');
$tester->start();
$tester->expectLogStartNotices();

$script = '/' . basename($tester->makeSourceFile());

$httpAddr = $tester->getAddr('ipv4', '[http]');
[$host, $port] = explode(':', $httpAddr);

function postBytes(string $host, int $port, string $script, int $length): string
{
    $fp = fsockopen($host, $port, $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    $head = "POST $script HTTP/1.1\r\nHost: $host\r\nContent-Length: $length\r\nConnection: close\r\n\r\n";
    fwrite($fp, $head . str_repeat('x', $length));
    $response = '';
    while (!feof($fp)) {
        $chunk = fgets($fp);
        if ($chunk === false) {
            break;
        }
        $response .= $chunk;
    }
    fclose($fp);
    return $response;
}

// Under the cap: the worker sees the request.
$small = postBytes($host, (int) $port, $script, 512);
if (!str_contains($small, ' 200 ') || !str_contains($small, 'len=512')) {
    echo "FAIL: 512-byte POST was not proxied:\n$small\n";
    exit(1);
}

// Over the 1k cap: evhttp answers 413 itself, the worker never runs.
$big = postBytes($host, (int) $port, $script, 2048);
if (!str_contains($big, ' 413 ')) {
    echo "FAIL: 2048-byte POST was not rejected with 413:\n$big\n";
    exit(1);
}
if (str_contains($big, 'len=2048')) {
    echo "FAIL: oversized POST reached the worker\n";
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
