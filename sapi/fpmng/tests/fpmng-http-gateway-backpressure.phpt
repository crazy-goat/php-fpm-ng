--TEST--
FPM http gateway: a client that does not read slows the upstream down instead of filling gateway memory (issue #596)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #596: the gateway used to read the upstream response as fast as the
 * worker produced it and keep every byte in the client's output buffer. Now,
 * past http.response_buffer unwritten bytes, it stops reading the upstream.
 *
 * The worker prints $mib MiB (one MiB per echo, each filled with a different
 * letter) and records how many MiB it has handed to the gateway in a file. A
 * worker that is blocked in its own write has stopped counting, so the file is
 * the measurement:
 *   - [paused]  the client sends the request and never reads: the worker must
 *     stall well short of the end although nothing closes the connection;
 *   - [resume]  the same client then reads everything: the body must be
 *     byte-exact, which also proves the paused upstream was resumed;
 *   - [closed]  a client that disconnects while the upstream is paused: the
 *     worker must still run to the end (the paused upstream is read again and
 *     drained) and the connection must leave connections_open;
 *   - [timeout] a client that never reads and is not closed by us:
 *     http.write_timeout closes it, and the worker also runs to the end.
 * HTTP/1.0 requests, so the body is close-delimited and needs no chunk parsing. */

const MIB = 1048576;
const TOTAL = 128;
/* The limit is 1 MiB; the kernel buffers on three hops add a few MiB (6-7
 * observed). 16 MiB still fails a gateway that keeps tens of MiB per client. */
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
http.write_timeout = 3000
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

$tmp = sys_get_temp_dir() . '/fpmng-backpressure-' . getmypid();

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
echo "paused: held back\n";

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
echo "resume: byte-exact\n";

/* [closed] */
if (!waitFor(fn() => connectionsOpen($operator) === 0, 10)) {
    echo "FAIL: [closed] connections_open is not 0 before the case\n";
    exit(1);
}
$pf = "$tmp-b";
@unlink($pf);
$fp = connect($host, $port);
request($fp, $host, $script, $pf);
$stalled = settle($pf);
if ($stalled < 1 || $stalled > STALL_MAX) {
    echo "FAIL: [closed] worker is at $stalled MiB before the disconnect\n";
    exit(1);
}
fclose($fp);
if (!waitFor(fn() => progress($pf) === TOTAL, 20)) {
    echo "FAIL: [closed] worker stuck at " . progress($pf) . " MiB after the client left\n";
    exit(1);
}
if (!waitFor(fn() => connectionsOpen($operator) === 0, 10)) {
    echo "FAIL: [closed] connections_open still above 0\n";
    exit(1);
}
echo "closed: drained\n";

/* [timeout] */
$pf = "$tmp-c";
@unlink($pf);
$fp = connect($host, $port);
request($fp, $host, $script, $pf);
$stalled = settle($pf);
if ($stalled < 1 || $stalled > STALL_MAX) {
    echo "FAIL: [timeout] worker is at $stalled MiB with the client not reading\n";
    exit(1);
}
if (!waitFor(fn() => connectionsOpen($operator) === 0, 15)) {
    echo "FAIL: [timeout] the stalled client is still connected after http.write_timeout\n";
    exit(1);
}
if (!waitFor(fn() => progress($pf) === TOTAL, 20)) {
    echo "FAIL: [timeout] worker stuck at " . progress($pf) . " MiB after the client was cut\n";
    exit(1);
}
fclose($fp);
echo "timeout: cut and drained\n";

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
paused: held back
resume: byte-exact
closed: drained
timeout: cut and drained
after: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
