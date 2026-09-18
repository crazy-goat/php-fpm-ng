--TEST--
fpm-ng: http gateway answers ping.path itself, before routing, even with http.front_controller set (issue #382)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php

require_once "tester.inc";

/* Before #382, http.front_controller made SCRIPT_NAME "/index.php" for every
 * request the gateway did not answer itself, so upstream's ping matcher
 * (which compares SCRIPT_NAME) never fired and the application received
 * /ping like any other request. This pins the fix: the gateway answers
 * ping.path itself, before fpm_http_build_request() ever runs, so the
 * front-controller rewrite never gets a chance to hide the probe from
 * upstream -- because upstream never sees it at all. */

function httpGet(string $url, ?array &$headers = null): string|false
{
    $ctx = stream_context_create(['http' => ['timeout' => 5, 'ignore_errors' => true]]);
    $body = @file_get_contents($url, false, $ctx);
    $headers = $http_response_header ?? [];
    return $body;
}

$docRoot = sys_get_temp_dir() . '/fpmng-http-gw-ping-' . getmypid();
@mkdir($docRoot, 0700, true);
$marker = "$docRoot/app.marker";
@unlink($marker);
file_put_contents(
    "$docRoot/index.php",
    '<?php file_put_contents(' . var_export($marker, true) . ', "1", FILE_APPEND); '
    . 'echo "app-saw:" . $_SERVER["REQUEST_URI"];'
);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
chdir = $docRoot
pm = static
pm.max_children = 1
pool.type = http
http.listen = {{ADDR[http]}}
http.front_controller = /index.php
ping.path = /ping
ping.response = pong
EOT;

$tester = new FPM\Tester($cfg, file_get_contents("$docRoot/index.php"));
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

$headers = [];
$body = httpGet("http://$http/ping", $headers);
if ($body !== 'pong') {
    echo 'FAIL: ping body=' . var_export($body, true) . "\n";
    exit(1);
}
$statusLine = $headers[0] ?? '';
if (!str_contains($statusLine, '200')) {
    echo "FAIL: ping status=$statusLine\n";
    exit(1);
}
$contentType = '';
foreach ($headers as $h) {
    if (stripos($h, 'Content-Type:') === 0) {
        $contentType = trim(substr($h, strlen('Content-Type:')));
    }
}
if ($contentType !== 'text/plain') {
    echo "FAIL: ping content-type=$contentType\n";
    exit(1);
}
if (is_file($marker)) {
    echo "FAIL: the application saw /ping\n";
    exit(1);
}
echo "ping-answered-locally: ok\n";

/* A normal request still reaches the app through the front controller,
 * proving http.front_controller itself is untouched by the fix. */
$appBody = httpGet("http://$http/missing-route");
if ($appBody !== 'app-saw:/missing-route' || !is_file($marker)) {
    echo 'FAIL: app request body=' . var_export($appBody, true) . "\n";
    exit(1);
}
echo "app-still-reachable: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink($marker);
@unlink("$docRoot/index.php");
@rmdir($docRoot);

?>
Done
--EXPECT--
ping-answered-locally: ok
app-still-reachable: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
