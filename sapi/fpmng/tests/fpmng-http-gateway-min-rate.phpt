--TEST--
FPM http gateway: a trickling client is cut by http.response_min_rate, a slow-but-progressing one is not (issue #705)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #705, the follow-up #596 left open. Holding the upstream back gives a
 * trickling client a way to hold a worker indefinitely: http.write_timeout is
 * a stall timer and restarts on every byte, so a client that reads one byte
 * per second is never cut by it. While the gateway has the upstream paused it
 * now also requires the client to drain at least http.response_min_rate bytes
 * per second over a fixed window (FPM_HTTP_RESPONSE_MIN_RATE_WINDOW_MS, 5 s).
 *
 * The worker prints $mib MiB and records the count in a file, so "the worker
 * is held" and "the worker was released" are observable:
 *   - [progress] a client that reads nothing until the worker stalls, then
 *     reads slowly but above the rate for longer than one window, must keep
 *     its connection and its worker: the response keeps arriving and no
 *     minimum-rate cut is logged;
 *   - [trickle]  a client that then reads one byte every 200 ms (far below the
 *     rate) must be cut within a window or two; the worker must still run to
 *     the end (the paused upstream is read again and drained) and the
 *     connection must leave connections_open.
 *
 * HTTP/1.0 requests, so the body is close-delimited and needs no chunk parsing.
 * http.write_timeout = 0 on purpose: the [trickle] cut must come from the
 * minimum rate, not from the stall timer that cannot see a trickle anyway. */

const MIB = 1048576;
const TOTAL = 32;
/* 16 KiB every 125 ms = 128 KiB/s: above the configured http.response_min_rate
 * (16384 bytes/s in the config below), and slow enough that the 1 MiB response
 * buffer takes about 8 s to drain, so a 5 s window elapses with the upstream
 * still paused. */
const READ_CHUNK = 16384;
const READ_SLEEP_US = 125000;

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
http.response_min_rate = 16384
http.write_timeout = 0
http.route[app] = /
operator.status_listen = {{ADDR[operator]}}
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

function connectionsOpen(string $operator): int
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

/* Waits until the worker's progress stops changing for 0.5 s, which means the
 * gateway has paused its upstream: the worker is blocked in its own write. */
function waitStall(string $file): int
{
    $last = -1;
    $since = microtime(true);
    while (microtime(true) - $since < 0.5) {
        $now = progress($file);
        if ($now !== $last) {
            $last = $now;
            $since = microtime(true);
        }
        usleep(50000);
    }
    return $last;
}

/* Skips the HTTP head, returns the stream once the first body byte is seen. */
function readHead($fp): string
{
    $head = '';
    while (strpos($head, "\r\n\r\n") === false) {
        $data = fread($fp, 4096);
        if ($data === false || $data === '') {
            echo "FAIL: connection closed before the response head\n";
            exit(1);
        }
        $head .= $data;
    }
    if (!str_starts_with($head, 'HTTP/1.')) {
        echo "FAIL: bad response head: " . substr($head, 0, 40) . "\n";
        exit(1);
    }
    return substr($head, strpos($head, "\r\n\r\n") + 4);
}

$tmp = sys_get_temp_dir() . '/fpmng-minrate-' . getmypid();

/* [progress]: reads above the rate for longer than one window; not cut. */
$pf = "$tmp-a";
@unlink($pf);
$fp = connect($host, $port);
request($fp, $host, $script, $pf);
$stalled = waitStall($pf);
if ($stalled < 1 || $stalled > TOTAL - 1) {
    echo "FAIL: [progress] worker is at $stalled of " . TOTAL . " MiB, no pause to test\n";
    exit(1);
}
stream_set_blocking($fp, true);
stream_set_timeout($fp, 5);
$body = readHead($fp);
$read = strlen($body);
$start = microtime(true);
while (microtime(true) - $start < 6.5) {
    $data = fread($fp, READ_CHUNK);
    if ($data === false || $data === '') {
        if (feof($fp) || stream_get_meta_data($fp)['timed_out']) {
            echo "FAIL: [progress] connection ended after " . ($read / MIB) . " MiB, want it kept\n";
            exit(1);
        }
    } else {
        $read += strlen($data);
    }
    usleep(READ_SLEEP_US);
}
if ($read < 128 * 1024) {
    echo "FAIL: [progress] read only $read bytes in 6.5 s, the response was not flowing\n";
    exit(1);
}
fclose($fp);
echo "progress: kept\n";
if (!waitFor(fn() => connectionsOpen($operator) === 0, 15)) {
    echo "FAIL: [progress] connections_open still above 0\n";
    exit(1);
}
if (!waitFor(fn() => progress($pf) === TOTAL, 20)) {
    echo "FAIL: [progress] worker stuck at " . progress($pf) . " MiB after the client left\n";
    exit(1);
}

/* [trickle]: one byte every 200 ms, far below the rate; cut within a window.
 *
 * The cut is observed through the operator's connections_open, not through EOF
 * on the client: the client reads one byte per 200 ms, so the bytes already in
 * its socket buffer would take hours to drain before it could see the FIN. The
 * gateway drops the connection when its own timer fires, and that is what
 * connections_open reports. */
$pf = "$tmp-b";
@unlink($pf);
$fp = connect($host, $port);
request($fp, $host, $script, $pf);
$stalled = waitStall($pf);
if ($stalled < 1 || $stalled > TOTAL - 1) {
    echo "FAIL: [trickle] worker is at $stalled of " . TOTAL . " MiB, no pause to test\n";
    exit(1);
}
stream_set_blocking($fp, true);
stream_set_timeout($fp, 5);
readHead($fp);
stream_set_blocking($fp, false);
$t0 = microtime(true);
$cutAt = null;
while (microtime(true) - $t0 < 20) {
    fread($fp, 1);
    usleep(200000);
    if (connectionsOpen($operator) === 0) {
        $cutAt = microtime(true) - $t0;
        break;
    }
}
fclose($fp);
if ($cutAt === null) {
    echo "FAIL: [trickle] the trickling client was not cut after 20 s\n";
    exit(1);
}
if ($cutAt > 12.0) {
    echo "FAIL: [trickle] cut after " . round($cutAt, 1) . " s, want within a window or two\n";
    exit(1);
}
echo "trickle: cut\n";
if (!waitFor(fn() => progress($pf) === TOTAL, 20)) {
    echo "FAIL: [trickle] worker stuck at " . progress($pf) . " MiB after the client was cut\n";
    exit(1);
}
echo "trickle: drained\n";

/* The minimum-rate cut is named in the log, once. */
$tester->expectLogPattern('/below http\.response_min_rate/');

/* The pool still answers normally. */
$fp = connect($host, $port);
$q = http_build_query(['mib' => 1, 'p' => "$tmp-c"]);
fwrite($fp, "GET $script?$q HTTP/1.0\r\nHost: $host\r\n\r\n");
$all = stream_get_contents($fp);
fclose($fp);
echo strlen($all) > MIB ? "after: ok\n" : "FAIL: [after] short answer\n";

foreach (['a', 'b', 'c'] as $s) {
    @unlink("$tmp-$s");
}

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
progress: kept
trickle: cut
trickle: drained
after: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
