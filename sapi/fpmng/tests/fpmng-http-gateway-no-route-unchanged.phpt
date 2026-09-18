--TEST--
fpm-ng: a gateway with no http.route[] keeps its pre-#341 log/metric shape, plus target=- (issue #341)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #341's acceptance criteria ask for a check that a gateway WITHOUT
 * http.route[] gets NO format change anywhere, beyond the one documented
 * optional field (a trailing "target=-" in the access log, and "target=<own
 * pool>" elsewhere since an unrouted gateway's only target is itself --
 * fpm_http_routes_build() always puts the pool's own listener in targets[0]).
 * This is the negative control for fpmng-http-route-target-access-log.phpt
 * and fpmng-http-route-target-metrics.phpt: nothing here mentions
 * http.route[] at all. */

$docroot = sys_get_temp_dir() . '/fpmng-http-noroute-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php',
    '<?php if (str_contains($_SERVER["REQUEST_URI"] ?? "", "slow")) { sleep(2); } echo "ok";');

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
[solo]
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 1
pool.type = http
http.gateways = 1
http.listen = {{ADDR[http]}}
http.front_controller = /index.php
http.access_log = {{FILE:LOG:ACC}}
pm.metrics_listen = {{ADDR[operator]}}
pm.metrics_path = /metrics
EOT;

$tester = new FPM\Tester($config, '<?php echo "unused";');
/* forceStderr = false: the assertions below read error_log back from the FILE
 * the config points at (the reject-path WARNING and the startup NOTICE), the
 * same arrangement fpmng-http-gateway-access-log-reopen.phpt uses -- with the
 * default forceStderr = true, -O sends everything to the master's stdout pipe
 * instead and the configured file stays empty. */
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();

/* Requirement 4: one NOTICE line per target. An unrouted gateway has exactly
 * one target -- itself -- so this is still exactly one line, same as before
 * #341, just present now where it previously was not printed at all. */
$errorLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
$errors = (string) @file_get_contents($errorLog);
if (!preg_match('/http target: pool solo at \S+, \d+ persistent connection\(s\)/', $errors)) {
    echo "FAIL: no per-target startup NOTICE line:\n$errors\n";
    exit(1);
}
if (substr_count($errors, 'http target: pool') !== 1) {
    echo "FAIL: expected exactly one http target NOTICE line:\n$errors\n";
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

// A plain served request: the access log line is unchanged CLF plus the
// trailing target=- field.
$body = readAll(request($host, (int) $port, '/index.php?r=plain'));
if (!str_contains($body, ' 200 ')) {
    echo "FAIL: plain request did not succeed:\n$body\n";
    exit(1);
}

$accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
$content = fileWait($accessLog, '/r=plain/');
$plainLine = null;
foreach (explode("\n", $content) as $line) {
    if (str_contains($line, 'r=plain')) {
        $plainLine = $line;
    }
}
if ($plainLine === null) {
    echo "FAIL: no access log line for the plain request:\n$content\n";
    exit(1);
}
if (!preg_match('/^\S+ - \S+ \[[^\]]+\] "GET \S+ HTTP\/\d\.\d" \d+ \d+ "[^"]*" "[^"]*" target=-$/', $plainLine)) {
    echo "FAIL: plain access log line is not CLF+target=-:\n$plainLine\n";
    exit(1);
}
echo "access-log: target=-\n";

// Pin the only worker, then trigger the reject-path WARNING.
$slow = request($host, (int) $port, '/index.php?slow=1');
stream_set_blocking($slow, false);
usleep(500000);

$rejected = readAll(request($host, (int) $port, '/index.php?r=rejected'));
if (!str_starts_with($rejected, 'HTTP/1.1 503') && !str_starts_with($rejected, 'HTTP/1.0 503')) {
    echo "FAIL: expected 503 on the full pool, got:\n$rejected\n";
    exit(1);
}
echo "reject: 503\n";

$errors = fileWait($errorLog, '/pool full, rejecting a queued request/');
if (!preg_match('/pool full, rejecting a queued request target=solo/', $errors)) {
    echo "FAIL: reject WARNING has no target=solo:\n$errors\n";
    exit(1);
}
echo "reject-warning: target=solo\n";

// Metrics: the only target is the gateway's own pool.
$metrics = fpmng_operator_body($operator, '/metrics');
foreach (['fpmng_gateway_upstreams_used{pool="solo",target="solo"}',
          'fpmng_gateway_upstreams_max{pool="solo",target="solo"}',
          'fpmng_gateway_requests_total{pool="solo",target="solo"}',
          'fpmng_gateway_rejected_total{pool="solo",target="solo"}'] as $needle) {
    if (!str_contains($metrics, $needle)) {
        echo "FAIL: metrics missing $needle:\n$metrics\n";
        exit(1);
    }
}
if (!preg_match('/fpmng_gateway_rejected_total\{pool="solo",target="solo"\} (\d+)/', $metrics, $m) || (int) $m[1] < 1) {
    echo "FAIL: rejected_total{target=solo} did not rise:\n$metrics\n";
    exit(1);
}
echo "metrics: ok\n";

$first = readAll($slow);
if (!str_contains($first, ' 200 ')) {
    echo "FAIL: the in-flight slow request did not complete:\n$first\n";
    exit(1);
}
echo "slow-request-completed: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink($docroot . '/index.php');
@rmdir($docroot);

?>
Done
--EXPECT--
startup-notice: ok
access-log: target=-
reject: 503
reject-warning: target=solo
metrics: ok
slow-request-completed: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
