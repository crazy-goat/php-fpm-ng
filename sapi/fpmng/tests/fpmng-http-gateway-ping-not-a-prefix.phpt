--TEST--
fpm-ng: http gateway ping.path is matched whole and un-decoded, so /pings and /%70ing reach the application (issue #382)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php

require_once "tester.inc";

/* The matcher is copied verbatim from http-direct (fpm_http_direct_ops.c):
 * whole-path comparison, so "/pings" is not "/ping", and no percent-decoding,
 * so "/%70ing" ("p" percent-encoded) is not a way to spell "/ping" past a
 * proxy rule that was written against the literal in the pool file. Both
 * must be forwarded to the application like any ordinary request, never
 * answered locally. */

function httpGet(string $url): string|false
{
    $ctx = stream_context_create(['http' => ['timeout' => 5, 'ignore_errors' => true]]);
    return @file_get_contents($url, false, $ctx);
}

$docRoot = sys_get_temp_dir() . '/fpmng-http-gw-ping-prefix-' . getmypid();
@mkdir($docRoot, 0700, true);
file_put_contents(
    "$docRoot/app.php",
    '<?php echo "app-saw:" . $_SERVER["REQUEST_URI"];'
);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
chdir = $docRoot
pm = static
pm.max_children = 2
pool.type = http
http.listen = {{ADDR[http]}}
http.front_controller = /app.php
ping.path = /ping
ping.response = pong
EOT;

$tester = new FPM\Tester($cfg, file_get_contents("$docRoot/app.php"));
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

$body = httpGet("http://$http/ping");
if ($body !== 'pong') {
    echo 'FAIL: /ping body=' . var_export($body, true) . "\n";
    exit(1);
}
echo "ping-still-answered-locally: ok\n";

$body = httpGet("http://$http/pings");
if ($body !== 'app-saw:/pings') {
    echo 'FAIL: /pings body=' . var_export($body, true) . "\n";
    exit(1);
}
echo "pings-not-ping: ok\n";

$body = httpGet("http://$http/%70ing");
if ($body !== 'app-saw:/%70ing') {
    echo 'FAIL: /%70ing body=' . var_export($body, true) . "\n";
    exit(1);
}
echo "percent-encoded-not-ping: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$docRoot/app.php");
@rmdir($docRoot);

?>
Done
--EXPECT--
ping-still-answered-locally: ok
pings-not-ping: ok
percent-encoded-not-ping: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
