--TEST--
fpm-ng: a gateway with no route claiming '/' answers 404 locally, logs target=-, and still serves routed prefixes (issue #388)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #388: the old pool.type = http gateway always had an implicit target 0
 * -- the pool's own listener at "/" -- so "no http.route[]" was a meaningful,
 * supported shape (issue #341 pinned its log/metric shape). The gateway type
 * has no own pool: the route table is exactly http.route[], a request that
 * matches none is a local 404 (never a forward; 502/503 would mean a target
 * exists and failed or is full), and the access log's target field is "-".
 * This test is that shape's positive control: nothing routes "/", yet the
 * gateway starts and serves the prefix that is routed. */

$docroot = sys_get_temp_dir() . '/fpmng-http-noroute-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo "ok";');

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.front_controller = /index.php
http.route[api] = /api
http.access_log = {{FILE:LOG:ACC}}
operator.metrics_listen = {{ADDR[operator]}}
operator.metrics_path = /metrics

[api]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = $docroot
EOT;

$tester = new FPM\Tester($config, '<?php echo "unused";');
/* forceStderr = false: the assertions below read error_log back from the FILE
 * the config points at (the startup NOTICE), the same arrangement
 * fpmng-http-gateway-access-log-reopen.phpt uses. */
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

/* The startup NOTICE says no route claims "/". */
$errorLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
$errors = (string) @file_get_contents($errorLog);
if (!preg_match('/no http\.route\[\] entry claims \'\/\'/', $errors)) {
    echo "FAIL: no 'no route claims /' NOTICE:\n$errors\n";
    exit(1);
}
echo "startup-notice: ok\n";

$httpAddr = $tester->getAddr('ipv4', '[http]');
[$host, $port] = explode(':', $httpAddr);
$operator = $tester->getListen('{{ADDR[operator]}}');

function request(string $host, int $port, string $path)
{
    $fp = fsockopen($host, $port, $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect for $path: $errstr ($errno)\n";
        exit(1);
    }
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
    return $fp;
}

function readAll($fp): string
{
    stream_set_blocking($fp, true);
    $out = '';
    while (!feof($fp)) {
        $chunk = fgets($fp);
        if ($chunk === false) {
            break;
        }
        $out .= $chunk;
    }
    fclose($fp);
    return $out;
}

function fileWait(string $file, string $pattern, int $seconds = 10): string
{
    $deadline = microtime(true) + $seconds;
    do {
        $content = (string) @file_get_contents($file);
        if (preg_match($pattern, $content)) {
            return $content;
        }
        usleep(100000);
    } while (microtime(true) < $deadline);

    return $content;
}

// A routed request still reaches its target.
$routed = readAll(request($host, (int) $port, '/api/index.php'));
if (!str_contains($routed, ' 200 ')) {
    echo "FAIL: routed request did not succeed:\n$routed\n";
    exit(1);
}
echo "routed-prefix: 200\n";

// An unrouted request is a local 404, never a forward.
$unrouted = readAll(request($host, (int) $port, '/other?r=unrouted'));
if (!str_starts_with($unrouted, 'HTTP/1.1 404') && !str_starts_with($unrouted, 'HTTP/1.0 404')) {
    echo "FAIL: expected 404 for an unrouted path, got:\n$unrouted\n";
    exit(1);
}
echo "unrouted: 404\n";

$accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
$content = fileWait($accessLog, '/r=unrouted/');
if (substr_count($content, 'target=-') < 1) {
    echo "FAIL: unrouted access log line did not carry target=-:\n$content\n";
    exit(1);
}
echo "access-log: target=-\n";

// Metrics: one target, api; the gateway's own pool is not a target at all.
$metrics = fpmng_operator_body($operator, '/metrics');
foreach (['fpmng_gateway_requests_total{pool="gw",target="api"}',
          'fpmng_gateway_rejected_total{pool="gw",target="api"}'] as $needle) {
    if (!str_contains($metrics, $needle)) {
        echo "FAIL: metrics missing $needle:\n$metrics\n";
        exit(1);
    }
}
if (!str_contains($metrics, 'fpmng_pool_info{pool="gw",type="gateway"}')) {
    echo "FAIL: metrics missing the gateway's own fpmng_pool_info:\n$metrics\n";
    exit(1);
}
echo "metrics: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink($docroot . '/index.php');
@rmdir($docroot);

?>
Done
--EXPECT--
startup-notice: ok
routed-prefix: 200
unrouted: 404
access-log: target=-
metrics: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
