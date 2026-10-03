--TEST--
FPM http gateway: a read that pauses the upstream and ends the request resumes it (issue #596, http.idle_timeout = 0)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* The whole response (head, body and END_REQUEST) fits in one 16 KiB read of
 * the upstream, and the body is larger than http.response_buffer, so the same
 * read pauses the upstream and finishes the request. With http.idle_timeout = 0
 * nothing but fpm_http_finish() re-arms the read event of the pinned upstream,
 * so the next request on the keep-alive connection would hang.
 *
 * Whether END_REQUEST lands in the same read as the body is a race between
 * the worker and the gateway, so the test repeats the request; against a
 * build without that resume it stalls within a few runs of 5 requests. */

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = __DIR__
http.gateways = 1
http.response_buffer = 1k
http.idle_timeout = 0
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR[app]}}
pm = static
pm.max_children = 1
chdir = __DIR__
EOT;
$cfg = str_replace('__DIR__', __DIR__, $cfg);

$code = <<<'PHP'
<?php
header('Content-Length: 8192');
echo str_repeat('x', 8192);
PHP;

$tester = new FPM\Tester($cfg, $code);
$tester->start();
$tester->expectLogStartNotices();
$script = '/' . basename($tester->makeSourceFile());
[$host, $port] = explode(':', $tester->getAddr('ipv4', '[http]'));

$fp = stream_socket_client("tcp://$host:$port", $errno, $errstr, 5);
if (!$fp) {
    echo "FAIL: connect: $errstr ($errno)\n";
    exit(1);
}
stream_set_timeout($fp, 5);
for ($n = 1; $n <= 200; $n++) {
    fwrite($fp, "GET $script HTTP/1.1\r\nHost: $host\r\n\r\n");
    $buf = '';
    while (($pos = strpos($buf, "\r\n\r\n")) === false || strlen($buf) < $pos + 4 + 8192) {
        $data = fread($fp, 65536);
        if ($data === false || $data === '') {
            echo "FAIL: request $n stalled after " . strlen($buf) . " bytes\n";
            exit(1);
        }
        $buf .= $data;
    }
}
echo "200 requests on one connection\n";
fclose($fp);

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
200 requests on one connection
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
