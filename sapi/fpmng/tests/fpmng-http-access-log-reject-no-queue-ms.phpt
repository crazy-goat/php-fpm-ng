--TEST--
fpm-ng: http.access_log has no queue_ms under pool_full_policy = reject, also for a 503 from the reclaim grace (issue #642)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #642: queue_ms is printed only under http.pool_full_policy = wait.
 * With two gateway processes and one worker, a slow request holds the worker
 * in one process. A request that reaches the OTHER process finds the budget
 * held by a sibling and waits for the reclaim grace (issue #735, 100 ms)
 * before it answers 503. That grace timer ends in the same callback as the
 * wait policy's bound, and must not put a queue_ms= value on that 503. A
 * request that reaches the slow process is answered 503 at once. Both kinds
 * of request must print no queue_ms=. */
$docroot = sys_get_temp_dir() . '/fpmng-acclog-reject-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo "web-ok";');
file_put_contents($docroot . '/slow.php', '<?php usleep(1500000); echo "slow-ok";');

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 2
http.front_controller = /index.php
http.access_log = {{FILE:LOG:ACC}}
http.pool_full_policy = reject
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

function openRequest(string $host, string $port, string $path): mixed
{
    $fp = fsockopen($host, (int) $port, $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect $path: $errstr ($errno)\n";
        exit(1);
    }
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: $host\r\nConnection: close\r\n\r\n");
    return $fp;
}

$slow = openRequest($host, $port, '/slow.php?r=slow');
usleep(300000);

$quickCount = 12;
$quick = [];
for ($i = 0; $i < $quickCount; $i++) {
    $quick[$i] = openRequest($host, $port, "/index.php?r=q$i");
}

$bad = [];
foreach ($quick as $i => $fp) {
    $response = stream_get_contents($fp);
    fclose($fp);
    if (!str_contains($response, ' 503 ')) {
        $bad[] = "q$i: " . strtok($response, "\r\n");
    }
}
$slowResponse = stream_get_contents($slow);
fclose($slow);
if ($bad) {
    echo "FAIL: quick requests not answered 503 while the worker was held:\n" . implode("\n", $bad) . "\n";
    exit(1);
}
if (!str_contains($slowResponse, ' 200 ')) {
    echo "FAIL: slow request was not answered 200: " . strtok($slowResponse, "\r\n") . "\n";
    exit(1);
}
echo "quick-503: $quickCount of $quickCount\n";

$accessLog = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
$content = '';
$deadline = microtime(true) + 10;
do {
    $content = (string) @file_get_contents($accessLog);
    $seen = 0;
    for ($i = 0; $i < $quickCount; $i++) {
        if (str_contains($content, "r=q$i ")) {
            $seen++;
        }
    }
    if ($seen === $quickCount && str_contains($content, 'r=slow ')) {
        break;
    }
    usleep(100000);
} while (microtime(true) < $deadline);

$lines = 0;
foreach (explode("\n", $content) as $line) {
    if (!str_contains($line, 'r=q') && !str_contains($line, 'r=slow ')) {
        continue;
    }
    $lines++;
    if (str_contains($line, 'queue_ms=')) {
        echo "FAIL: reject gateway printed queue_ms:\n$line\n";
        exit(1);
    }
    if (!str_contains($line, ' duration_ms=') || !str_contains($line, ' upstream_ms=')) {
        echo "FAIL: line without the timing fields:\n$line\n";
        exit(1);
    }
}
if ($lines < $quickCount + 1) {
    echo "FAIL: only $lines of " . ($quickCount + 1) . " access log lines:\n$content\n";
    exit(1);
}
echo "no-queue-ms: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$docroot/index.php");
@unlink("$docroot/slow.php");
@rmdir($docroot);
echo "Done\n";
?>
--EXPECT--
quick-503: 12 of 12
no-queue-ms: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
