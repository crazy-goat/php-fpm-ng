--TEST--
FPM gateway: a response paused for a client that does not read is counted and shown as paused (issue #706)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #706. Flow control (#596) pauses the upstream read of a response
 * whose client has not read http.response_buffer bytes. The pause was not
 * observable. Two numbers show it now:
 *   - fpmng_gateway_responses_paused_total, a pool-wide counter of pauses;
 *   - fpmng_gateway_responses_paused, a gauge per gateway process of the
 *     pauses in progress right now.
 * The test drives one slow client through four stages. It polls /metrics in
 * each stage, because a gauge is only proven by a later read of it:
 *   - [paused]  the client sends the request and never reads: the counter
 *     rises and the gauge reads 1;
 *   - [resume]  the client reads everything: the body is byte-exact and the
 *     gauge returns to 0;
 *   - [closed]  the client leaves while the response is paused: the gauge
 *     returns to 0 and the worker still runs to the end;
 *   - [killed]  the gateway is SIGKILLed while the response is paused: the
 *     master zeroes the dead process's gauge block, the gauge returns to 0
 *     and the counter survives the respawn.
 * http.response_min_rate is 0 here. The #705 cut is tested on its own, and it
 * would close the stalled client before the gauge is read. */

const MIB = 1048576;
const TOTAL = 128;
/* The limit is 1 MiB; the kernel buffers on three hops add a few MiB (see
 * fpmng-http-gateway-backpressure.phpt). */
const STALL_MAX = 16;

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 5
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = __DIR__
http.gateways = 1
http.response_buffer = 1M
http.response_min_rate = 0
http.route[app] = /
operator.metrics_listen = {{ADDR[operator]}}
[app]
pool.type = fastcgi
listen = {{ADDR[app]}}
pm = static
pm.max_children = 2
chdir = __DIR__
EOT;
$cfg = str_replace('__DIR__', __DIR__, $cfg);

$code = <<<'PHP'
<?php
$mib = (int) $_GET['mib'];
$progress = $_GET['p'];
for ($i = 0; $i < $mib; $i++) {
    echo str_repeat(chr(65 + $i % 26), 1048576);
    file_put_contents($progress, (string) ($i + 1));
}
PHP;

$tester = new FPM\Tester($cfg, $code);
$tester->start();
$tester->expectLogStartNotices();

$script = '/' . basename($tester->makeSourceFile());
$httpAddr = $tester->getAddr('ipv4', '[http]');
$operator = $tester->getListen('{{ADDR[operator]}}');
[$host, $port] = explode(':', $httpAddr);

function connect(string $host, string $port)
{
    $ctx = stream_context_create(['socket' => ['so_rcvbuf' => 4096]]);
    $fp = stream_socket_client("tcp://$host:$port", $errno, $errstr, 5, STREAM_CLIENT_CONNECT, $ctx);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    return $fp;
}

function progress(string $file): int
{
    return (int) @file_get_contents($file);
}

function request($fp, string $host, string $script, string $progressFile): void
{
    $q = http_build_query(['mib' => TOTAL, 'p' => $progressFile]);
    fwrite($fp, "GET $script?$q HTTP/1.0\r\nHost: $host\r\n\r\n");
}

/* [gauge, counter] of the pool "gw" from /metrics. */
function pausedMetrics(string $operator): array
{
    $metrics = fpmng_operator_body($operator, '/metrics');
    if (!preg_match('/^fpmng_gateway_responses_paused\{pool="gw"\} (\d+)$/m', $metrics, $g)
        || !preg_match('/^fpmng_gateway_responses_paused_total\{pool="gw"\} (\d+)$/m', $metrics, $t)) {
        echo "FAIL: responses_paused lines are missing from /metrics\n$metrics\n";
        exit(1);
    }
    return [(int) $g[1], (int) $t[1]];
}

/* Polls /metrics until $cond([gauge, counter]) holds, and returns the last
 * reading either way. */
function pollPaused(string $operator, callable $cond, float $max): array
{
    $start = microtime(true);
    do {
        $now = pausedMetrics($operator);
        if ($cond($now)) {
            return $now;
        }
        usleep(100000);
    } while (microtime(true) - $start < $max);
    return $now;
}

/* The gateway processes of THIS master (ppid = its pid from the pid file); a
 * bare args match would find another php-fpm-ng instance's "gw" pool. */
function gatewayPids(int $masterPid, ?int $except = null): array
{
    $pids = [];
    foreach (explode("\n", (string) shell_exec('ps -eo pid,ppid,args 2>/dev/null')) as $line) {
        if (preg_match('/^\s*(\d+)\s+(\d+)\s+(.*)$/', $line, $m)
            && (int) $m[2] === $masterPid && str_contains($m[3], 'http gateway gw')
            && (int) $m[1] !== $except) {
            $pids[] = (int) $m[1];
        }
    }
    return $pids;
}

function waitFor(callable $cond, float $max): bool
{
    $start = microtime(true);
    while (microtime(true) - $start < $max) {
        if ($cond()) {
            return true;
        }
        usleep(100000);
    }
    return false;
}

/* Waits until the file stops changing for 1.5 s, returns the last value. */
function settle(string $file): int
{
    $last = -1;
    $since = microtime(true);
    while (microtime(true) - $since < 1.5) {
        $now = progress($file);
        if ($now !== $last) {
            $last = $now;
            $since = microtime(true);
        }
        usleep(100000);
    }
    return $last;
}

$tmp = sys_get_temp_dir() . '/fpmng-paused-metrics-' . getmypid();

[$gauge0, $counter0] = pausedMetrics($operator);
if ($gauge0 !== 0) {
    echo "FAIL: responses_paused is $gauge0 before any request\n";
    exit(1);
}

/* [paused] + [resume] */
$pf = "$tmp-a";
@unlink($pf);
$fp = connect($host, $port);
request($fp, $host, $script, $pf);
$stalled = settle($pf);
if ($stalled < 1 || $stalled > STALL_MAX) {
    echo "FAIL: [paused] worker is at $stalled of " . TOTAL . " MiB with the client not reading\n";
    exit(1);
}
[$gauge, $counter] = pollPaused($operator, fn($m) => $m[0] === 1 && $m[1] > $counter0, 10);
if ($gauge !== 1 || $counter <= $counter0) {
    echo "FAIL: [paused] responses_paused=$gauge responses_paused_total=$counter (was $counter0)"
        . " with the client not reading\n";
    exit(1);
}
echo "paused: counted and shown\n";

$expected = hash_init('md5');
for ($i = 0; $i < TOTAL; $i++) {
    hash_update($expected, str_repeat(chr(65 + $i % 26), MIB));
}
$got = hash_init('md5');
$head = '';
$bodyBytes = 0;
stream_set_timeout($fp, 20);
while (!feof($fp)) {
    $data = fread($fp, 262144);
    if ($data === false || ($data === '' && stream_get_meta_data($fp)['timed_out'])) {
        echo "FAIL: [resume] read stalled after $bodyBytes bytes\n";
        exit(1);
    }
    if ($head !== null) {
        $head .= $data;
        $pos = strpos($head, "\r\n\r\n");
        if ($pos === false) {
            continue;
        }
        if (!str_starts_with($head, 'HTTP/1.')) {
            echo "FAIL: [resume] bad head\n";
            exit(1);
        }
        $data = substr($head, $pos + 4);
        $head = null;
    }
    hash_update($got, $data);
    $bodyBytes += strlen($data);
}
fclose($fp);
if ($bodyBytes !== TOTAL * MIB || hash_final($got) !== hash_final($expected)) {
    echo "FAIL: [resume] body is $bodyBytes bytes, want " . TOTAL * MIB . " byte-exact\n";
    exit(1);
}
[$gauge, $counter] = pollPaused($operator, fn($m) => $m[0] === 0, 10);
if ($gauge !== 0) {
    echo "FAIL: [resume] responses_paused is $gauge after the client read everything\n";
    exit(1);
}
echo "resume: byte-exact, gauge back to 0\n";

/* [closed] */
$pf = "$tmp-b";
@unlink($pf);
$fp = connect($host, $port);
request($fp, $host, $script, $pf);
$stalled = settle($pf);
if ($stalled < 1 || $stalled > STALL_MAX) {
    echo "FAIL: [closed] worker is at $stalled MiB before the disconnect\n";
    exit(1);
}
[$gauge] = pollPaused($operator, fn($m) => $m[0] === 1, 10);
if ($gauge !== 1) {
    echo "FAIL: [closed] responses_paused is $gauge before the disconnect\n";
    exit(1);
}
fclose($fp);
[$gauge] = pollPaused($operator, fn($m) => $m[0] === 0, 10);
if ($gauge !== 0) {
    echo "FAIL: [closed] responses_paused is still $gauge after the client left\n";
    exit(1);
}
if (!waitFor(fn() => progress($pf) === TOTAL, 20)) {
    echo "FAIL: [closed] worker stuck at " . progress($pf) . " MiB after the client left\n";
    exit(1);
}
echo "closed: gauge back to 0, worker drained\n";

/* [killed] */
$pf = "$tmp-c";
@unlink($pf);
$fp = connect($host, $port);
request($fp, $host, $script, $pf);
$stalled = settle($pf);
if ($stalled < 1 || $stalled > STALL_MAX) {
    echo "FAIL: [killed] worker is at $stalled of " . TOTAL . " MiB with the client not reading\n";
    exit(1);
}
[$gauge, $before] = pollPaused($operator, fn($m) => $m[0] === 1, 10);
if ($gauge !== 1) {
    echo "FAIL: [killed] responses_paused is $gauge before the kill\n";
    exit(1);
}

$masterPid = $tester->getPid();
$own = gatewayPids($masterPid);
if (count($own) !== 1) {
    echo "FAIL: [killed] expected exactly one gateway process, found " . count($own) . "\n";
    exit(1);
}
shell_exec('kill -9 ' . $own[0]);
fclose($fp);

/* The master respawns the gateway and zeroes the dead process's gauge block.
 * Wait for the new pid first, then for the gauge to drop. */
$deadline = microtime(true) + 15;
do {
    if (gatewayPids($masterPid, $own[0])) {
        break;
    }
    usleep(100000);
} while (microtime(true) < $deadline);

[$gauge, $after] = pollPaused($operator, fn($m) => $m[0] === 0, 15);
if ($gauge !== 0) {
    echo "FAIL: [killed] responses_paused is $gauge after the gateway was killed while paused\n";
    exit(1);
}
if ($after < $before) {
    echo "FAIL: [killed] responses_paused_total fell from $before to $after across the respawn\n";
    exit(1);
}
echo "killed: gauge back to 0, counter kept\n";

/* the pool still answers */
$fp = connect($host, $port);
$q = http_build_query(['mib' => 1, 'p' => "$tmp-d"]);
fwrite($fp, "GET $script?$q HTTP/1.0\r\nHost: $host\r\n\r\n");
$all = stream_get_contents($fp);
fclose($fp);
echo strlen($all) > MIB ? "after: ok\n" : "FAIL: [after] short answer\n";

foreach (['a', 'b', 'c', 'd'] as $s) {
    @unlink("$tmp-$s");
}

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
paused: counted and shown
resume: byte-exact, gauge back to 0
closed: gauge back to 0, worker drained
killed: gauge back to 0, counter kept
after: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
