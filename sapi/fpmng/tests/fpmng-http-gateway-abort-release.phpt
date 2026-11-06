--TEST--
fpm-ng: HTTP gateway frees the request that a client abort or a truncated reply detaches (issue #996)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux /proc');
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #996. libevent detaches a request whose connection dies before the
 * request is answered, and does not free it. Two gateway paths left that
 * request behind, with its headers and its whole body:
 *   - [abort]  the client closes while the reply is under way;
 *   - [trunc]  the worker dies after the reply head went out, so the gateway
 *              cuts the client connection (fpm_http_finish_truncated()).
 * Each request sends an 8 MiB body, which the gateway buffers whole before it
 * dispatches, so one leaked request costs about 8 MiB. Eight leaked requests
 * per case cost about 64 MiB of VmRSS. The limit is 16 MiB. Growth is read only
 * after connections_open is back at 0, so the close callback has run.
 *
 * pm.max_children is the gateway's upstream budget for the target. An aborted
 * request keeps its upstream until its worker finishes (one second here), so
 * nine aborts in a row need more than eight slots; 16 leaves room. */

const BODY = 8 * 1024 * 1024;
const ROUNDS = 8;
const LIMIT_KB = 16 * 1024;

$root = sys_get_temp_dir() . '/fpmng-http-gateway-abort-release';
@mkdir($root, 0700, true);
/* The worker keeps the reply open for a second, so the client's close lands
 * while the reply is still under way. */
file_put_contents("$root/abort.php", <<<'PHP'
<?php
file_get_contents('php://input');
header('Content-Type: text/plain');
echo str_repeat('x', 4096);
flush();
usleep(1000000);
PHP);
/* A head and a body fragment, then the worker is killed: the gateway has a
 * reply under way that can never be finished. Same shape as the kill in
 * fpmng-http-gateway-upstream-loss.phpt. */
file_put_contents("$root/die.php", <<<'PHP'
<?php
file_get_contents('php://input');
header('Content-Type: text/plain');
echo 'partial';
flush();
exec('kill -9 ' . getmypid());
PHP);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.route[app] = /
operator.status_listen = {{ADDR[operator]}}
[app]
pool.type = fastcgi
listen = {{ADDR[app]}}
chdir = $root
pm = static
pm.max_children = 16
EOT;

/* The gateway process whose parent is $master, via the setproctitle line
 * fpm_http_gateway_run() installs. 0 when none exists. */
function gateway_pid(int $master): int
{
    foreach (glob('/proc/[0-9]*/stat') as $stat) {
        $raw = @file_get_contents($stat);
        if ($raw === false || !preg_match('/^(\d+) \((.*)\) \S (\d+) /s', $raw, $m)) {
            continue;
        }
        if ((int) $m[3] !== $master) {
            continue;
        }
        $cmd = @file_get_contents(dirname($stat) . '/cmdline');
        if ($cmd !== false && str_contains($cmd, 'http gateway gw')) {
            return (int) $m[1];
        }
    }
    return 0;
}

function rss_kb(int $pid): int
{
    $status = @file_get_contents("/proc/$pid/status");
    if ($status === false || !preg_match('/^VmRSS:\s+(\d+) kB/m', $status, $m)) {
        echo "FAIL: no VmRSS for the gateway process $pid\n";
        exit(1);
    }
    return (int) $m[1];
}

function connections_open(string $operator): int
{
    $decoded = json_decode(fpmng_operator_body($operator, '/status'), true, flags: JSON_THROW_ON_ERROR);
    foreach ($decoded['pools'] as $row) {
        if (!array_key_exists('target', $row) && ($row['name'] ?? '') === 'gw') {
            return (int) $row['connections_open'];
        }
    }
    echo "FAIL: no pool row for gw\n";
    exit(1);
}

/* Returns once the gateway has no open client connection left, then a short
 * pause for the free that follows the gauge decrement in the close callback. */
function wait_idle(string $operator): void
{
    for ($i = 0; $i < 100; $i++) {
        if (connections_open($operator) === 0) {
            usleep(300000);
            return;
        }
        usleep(100000);
    }
    echo "FAIL: connections_open still above 0 after 10 s\n";
    exit(1);
}

/* Sends the request with its whole body, reads the reply head, closes. */
function abort_mid_reply(string $host, string $port, string $path, string $body): void
{
    $fp = stream_socket_client("tcp://$host:$port", $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    stream_set_timeout($fp, 10);
    $req = "POST $path HTTP/1.1\r\nHost: $host\r\nContent-Type: application/octet-stream\r\n"
        . 'Content-Length: ' . strlen($body) . "\r\n\r\n" . $body;
    if (fwrite($fp, $req) !== strlen($req)) {
        echo "FAIL: the request was not written in full\n";
        exit(1);
    }
    $head = '';
    while (!str_contains($head, "\r\n\r\n")) {
        $line = fgets($fp);
        if ($line === false) {
            echo "FAIL: no reply head for $path: " . var_export($head, true) . "\n";
            exit(1);
        }
        $head .= $line;
    }
    if (!str_starts_with($head, 'HTTP/1.1 200')) {
        echo "FAIL: unexpected reply for $path:\n$head\n";
        exit(1);
    }
    fclose($fp);
}

/* Sends the request, reads until the gateway cuts the connection. */
function truncated_reply(string $host, string $port, string $path, string $body): void
{
    $fp = stream_socket_client("tcp://$host:$port", $errno, $errstr, 5);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    stream_set_timeout($fp, 10);
    fwrite($fp, "POST $path HTTP/1.1\r\nHost: $host\r\nContent-Type: application/octet-stream\r\n"
        . 'Content-Length: ' . strlen($body) . "\r\n\r\n" . $body);
    stream_get_contents($fp);
    fclose($fp);
}

$tester = new FPM\Tester($cfg, '<?php');
$tester->start();
$tester->expectLogStartNotices();

$master = $tester->getPid();
$http = $tester->getAddr('ipv4', '[http]');
$operator = $tester->getListen('{{ADDR[operator]}}');
[$host, $port] = explode(':', $http);

$gw = gateway_pid($master);
if ($gw === 0) {
    echo "FAIL: the gateway process was not found\n";
    exit(1);
}

$body = str_repeat('x', BODY);

/* [abort] The first request warms the allocator, so the baseline is taken
 * after its memory is in place. */
abort_mid_reply($host, $port, '/abort.php', $body);
wait_idle($operator);
$base = rss_kb($gw);
for ($i = 0; $i < ROUNDS; $i++) {
    abort_mid_reply($host, $port, '/abort.php', $body);
}
wait_idle($operator);
$grow = rss_kb($gw) - $base;
if ($grow > LIMIT_KB) {
    echo sprintf("FAIL: [abort] gateway VmRSS grew by %d kB over %d aborted requests (limit %d kB)\n",
        $grow, ROUNDS, LIMIT_KB);
    exit(1);
}
echo "abort: released\n";

/* [trunc] */
truncated_reply($host, $port, '/die.php', $body);
wait_idle($operator);
$base = rss_kb($gw);
for ($i = 0; $i < ROUNDS; $i++) {
    truncated_reply($host, $port, '/die.php', $body);
}
wait_idle($operator);
$grow = rss_kb($gw) - $base;
if ($grow > LIMIT_KB) {
    echo sprintf("FAIL: [trunc] gateway VmRSS grew by %d kB over %d truncated replies (limit %d kB)\n",
        $grow, ROUNDS, LIMIT_KB);
    exit(1);
}
echo "trunc: released\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
abort: released
trunc: released
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$root = sys_get_temp_dir() . '/fpmng-http-gateway-abort-release';
@unlink("$root/abort.php");
@unlink("$root/die.php");
@rmdir($root);
?>
