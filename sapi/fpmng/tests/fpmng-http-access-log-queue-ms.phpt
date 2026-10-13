--TEST--
fpm-ng: http.access_log carries queue_ms, the same value as X-Fpmng-Queue-Wait, under pool_full_policy = wait (issue #642)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #642: queue_ms is the trailing field that matches the
 * X-Fpmng-Queue-Wait header (issue #309). Every request under
 * http.pool_full_policy = wait is queued first, so it prints queue_ms, 0 for
 * a request that found a free worker at once. Here one slow request holds the
 * only worker for about a second, and the second request waits behind it.
 * Both values come from the same measurement, so they must be equal. */
$docroot = sys_get_temp_dir() . '/fpmng-acclog-queue-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo "web-ok";');
file_put_contents($docroot . '/slow.php', '<?php usleep(1000000); echo "slow-ok";');

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 1
http.front_controller = /index.php
http.access_log = {{FILE:LOG:ACC}}
http.pool_full_policy = wait
http.pool_full_queue_max = 8
http.pool_full_wait_ms = 5000
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 1
EOT;

$tester = new FPM\Tester($config, '<?php echo "unused";');
$tester->start();
$tester->expectLogStartNotices();

$httpAddr = $tester->getAddr('ipv4', '[http]');
[$host, $port] = explode(':', $httpAddr);

function rawGet($fp, string $host, string $path): void
{
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
}

$slow = fsockopen($host, (int) $port, $errno, $errstr, 5);
$queued = fsockopen($host, (int) $port, $errno, $errstr, 5);
if (!$slow || !$queued) {
    echo "FAIL: connect: $errstr ($errno)\n";
    exit(1);
}
rawGet($slow, $host, '/slow.php?r=slow');
usleep(200000);
rawGet($queued, $host, '/index.php?r=queued');

$slowResponse = stream_get_contents($slow);
$queuedResponse = stream_get_contents($queued);
fclose($slow);
fclose($queued);

if (!str_contains($queuedResponse, ' 200 ')) {
    echo "FAIL: queued request was not answered 200:\n" . strtok($queuedResponse, "\r\n") . "\n";
    exit(1);
}
if (!preg_match('/^X-Fpmng-Queue-Wait: (\d+)/mi', $queuedResponse, $m)) {
    echo "FAIL: queued response has no X-Fpmng-Queue-Wait header\n";
    exit(1);
}
$headerWait = (int) $m[1];
if ($headerWait < 500) {
    echo "FAIL: queued request waited $headerWait ms, expected about 1000\n";
    exit(1);
}
echo "header-wait: long\n";

$accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
$content = '';
$deadline = microtime(true) + 10;
do {
    $content = (string) @file_get_contents($accessLog);
    if (str_contains($content, 'r=queued') && str_contains($content, 'r=slow')) {
        break;
    }
    usleep(100000);
} while (microtime(true) < $deadline);

$queuedLine = null;
$slowLine = null;
foreach (explode("\n", $content) as $line) {
    if (str_contains($line, 'r=queued')) {
        $queuedLine = $line;
    }
    if (str_contains($line, 'r=slow')) {
        $slowLine = $line;
    }
}
if ($queuedLine === null || $slowLine === null) {
    echo "FAIL: missing access log line(s):\n$content\n";
    exit(1);
}

if (!preg_match('/ target=web duration_ms=\d+ upstream_ms=\d+ queue_ms=(\d+)$/', $queuedLine, $q)) {
    echo "FAIL: queued line has no queue_ms trailer:\n$queuedLine\n";
    exit(1);
}
if ((int) $q[1] !== $headerWait) {
    echo "FAIL: queue_ms={$q[1]} differs from X-Fpmng-Queue-Wait: $headerWait\n";
    exit(1);
}
echo "log-queue-ms: equal to header\n";

if (!preg_match('/ target=web duration_ms=\d+ upstream_ms=\d+ queue_ms=\d+$/', $slowLine)) {
    echo "FAIL: slow line has no queue_ms trailer:\n$slowLine\n";
    exit(1);
}
echo "slow-line: queue_ms printed\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$docroot/index.php");
@unlink("$docroot/slow.php");
@rmdir($docroot);
echo "Done\n";
?>
--EXPECT--
header-wait: long
log-queue-ms: equal to header
slow-line: queue_ms printed
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
