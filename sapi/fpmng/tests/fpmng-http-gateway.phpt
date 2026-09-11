--TEST--
fpm-ng: HTTP gateway serves static files without PHP, runs scripts, and routes front_controller / PATH_INFO (docs/NOTES.md §6, task 018)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-tester.inc";

function httpGet(string $url): string|false
{
    $ctx = stream_context_create(['http' => ['timeout' => 5]]);
    return @file_get_contents($url, false, $ctx);
}

$docRoot = sys_get_temp_dir() . '/fpmng-http-gateway-' . getmypid();
@mkdir($docRoot, 0700, true);
$marker = "$docRoot/worker.marker";
file_put_contents("$docRoot/static.txt", 'static-bytes');
file_put_contents(
    "$docRoot/hit.php",
    '<?php file_put_contents(' . var_export($marker, true) . ', "php\n", FILE_APPEND); echo "php-body";'
);
file_put_contents("$docRoot/script.php", '<?php echo $_SERVER["PATH_INFO"] ?? "";');
file_put_contents("$docRoot/index.php", '<?php echo "fc:" . ($_SERVER["PATH_INFO"] ?? "");');

@unlink($marker);

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
http.static = 1
http.front_controller = /index.php
EOT;

$tester = new FPM\Tester($cfg, file_get_contents("$docRoot/hit.php"));
$tester->start();
fpmng_expect_log_start_notices($tester);
$http = $tester->getAddr('ipv4', '[http]');

$static = httpGet("http://$http/static.txt");
if ($static !== 'static-bytes') {
    echo 'FAIL: static file body=' . var_export($static, true) . "\n";
    exit(1);
}
if (is_file($marker)) {
    echo "FAIL: static request woke a worker\n";
    exit(1);
}
echo "static-without-worker: ok\n";

$phpBody = httpGet("http://$http/hit.php");
if ($phpBody !== 'php-body' || !is_file($marker)) {
    echo "FAIL: php script body=" . var_export($phpBody, true) . ' marker=' . (is_file($marker) ? 'yes' : 'no') . "\n";
    exit(1);
}
echo "php-script: ok\n";

$pathInfo = httpGet("http://$http/script.php/extra/bit");
if ($pathInfo !== '/extra/bit') {
    echo 'FAIL: PATH_INFO split=' . var_export($pathInfo, true) . "\n";
    exit(1);
}
echo "path-info-split: ok\n";

$fallback = httpGet("http://$http/missing-route");
if ($fallback !== 'fc:/missing-route') {
    echo 'FAIL: front_controller fallback=' . var_export($fallback, true) . "\n";
    exit(1);
}
echo "front-controller-on: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

$cfgOff = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web2]
listen = {{ADDR}}
chdir = $docRoot
pm = static
pm.max_children = 1
pool.type = http
http.listen = {{ADDR[http]}}
http.static = 1
http.front_controller =
EOT;

$tester2 = new FPM\Tester($cfgOff, file_get_contents("$docRoot/hit.php"));
$tester2->start();
fpmng_expect_log_start_notices($tester2);
$http2 = $tester2->getAddr('ipv4', '[http]');
$headers = @get_headers("http://$http2/missing-route");
$statusLine = is_array($headers) ? ($headers[0] ?? '') : '';
if (!str_contains($statusLine, '404')) {
    echo "FAIL: front_controller off expected 404, got $statusLine\n";
    exit(1);
}
echo "front-controller-off: ok\n";
$tester2->terminate();
$tester2->expectLogTerminatingNotices();
$tester2->close();

@unlink($marker);
@unlink("$docRoot/static.txt");
@unlink("$docRoot/hit.php");
@unlink("$docRoot/script.php");
@unlink("$docRoot/index.php");
@rmdir($docRoot);

?>
Done
--EXPECT--
static-without-worker: ok
php-script: ok
path-info-split: ok
front-controller-on: ok
front-controller-off: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
