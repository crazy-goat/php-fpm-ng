--TEST--
fpm-ng: an http.route target runs out of budget on its own, per pool and not per prefix (issue #340)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #340, acceptance criteria 2 and 4. The connection budget belongs to a
 * TARGET POOL, not to a prefix: two prefixes pointing at the same pool share
 * one pm.max_children and go full together, and a full target does not take
 * the rest of the gateway down with it.
 *
 * pm.max_children = 1 on the target and one gateway, the arrangement
 * fpmng-http-pool-full-503.phpt already uses: the first slow request pins the
 * only upstream connection, so anything else aimed at that pool finds no
 * budget and is answered 503 + Retry-After straight away. */

$docroot = sys_get_temp_dir() . '/fpmng-http-route-full-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php',
    '<?php if (str_contains($_SERVER["REQUEST_URI"] ?? "", "slow")) { sleep(2); } echo getenv("FPMNG_ROUTE_POOL") ?: "no-marker";');

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
http.route[events] = /sse,/stream
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 2
env[FPMNG_ROUTE_POOL] = web

[events]
listen = {{ADDR[events]}}
pm = static
pm.max_children = 1
env[FPMNG_ROUTE_POOL] = events
EOT;

$tester = new FPM\Tester($config, '<?php echo "unused";');
$tester->start();
$tester->expectLogStartNotices();

$httpAddr = $tester->getAddr('ipv4', '[http]');
[$host, $port] = explode(':', $httpAddr);

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

// Both prefixes reach the same pool while it has room.
foreach (['/sse/one', '/stream/one'] as $path) {
    $body = readAll(request($host, (int) $port, $path));
    if (!str_contains($body, ' 200 ') || !str_contains($body, 'events')) {
        echo "FAIL: $path was not served by the events pool:\n$body\n";
        exit(1);
    }
    echo "$path -> events\n";
}

// Pin the single events worker through one of the two prefixes.
$slow = request($host, (int) $port, '/sse/slow');
stream_set_blocking($slow, false);
usleep(500000);

/* Criterion 2: the budget is the pool's, so the OTHER prefix of the same pool
 * is full as well -- a per-prefix budget would have let /stream through. */
foreach (['/sse/second', '/stream/second'] as $path) {
    $start = microtime(true);
    $response = readAll(request($host, (int) $port, $path));
    $elapsed = microtime(true) - $start;
    if (!str_starts_with($response, 'HTTP/1.1 503') && !str_starts_with($response, 'HTTP/1.0 503')) {
        echo "FAIL: expected a 503 on the full target for $path, got:\n$response\n";
        exit(1);
    }
    if (!preg_match('/^Retry-After:\s*\d+/mi', $response)) {
        echo "FAIL: the 503 for $path came without a Retry-After header:\n$response\n";
        exit(1);
    }
    if ($elapsed > 3) {
        echo sprintf("FAIL: the 503 for %s was not immediate (%.1f s)\n", $path, $elapsed);
        exit(1);
    }
    echo "$path -> 503\n";
}

/* Criterion 4: the gateway's own pool has its own budget and keeps serving. */
$body = readAll(request($host, (int) $port, '/'));
if (!str_contains($body, ' 200 ') || !str_contains($body, 'web')) {
    echo "FAIL: / stopped being served while the events pool was full:\n$body\n";
    exit(1);
}
echo "/ -> web\n";

$first = readAll($slow);
if (!str_contains($first, ' 200 ') || !str_contains($first, 'events')) {
    echo "FAIL: the in-flight request did not complete:\n$first\n";
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
/sse/one -> events
/stream/one -> events
/sse/second -> 503
/stream/second -> 503
/ -> web
slow-request-completed: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
