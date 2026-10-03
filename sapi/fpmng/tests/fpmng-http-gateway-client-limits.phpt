--TEST--
FPM http gateway: keep-alive idle, second-request slowloris and slow-reader clients are cut off (issue #593)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-operator.inc";

/* Issue #593: before this, a gateway client was time-limited only until its
 * FIRST request was read. Three connections that must now end:
 *   - [idle]   one keep-alive request, then silence: http.keepalive_timeout;
 *   - [second] a second request trickled one byte at a time: it must end
 *     http.read_timeout after its FIRST byte, not http.keepalive_timeout
 *     after the previous response (the two are 3000 and 1500 ms, so the two
 *     causes cannot be confused);
 *   - [reader] a client that asks for 16 MiB and never reads it:
 *     http.write_timeout; connections_open must go back to 0.
 * Each case first proves the connection was alive and serving, so a gateway
 * that never answered cannot pass for the wrong reason. */

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
http.read_timeout = 1500
http.keepalive_timeout = 3000
http.write_timeout = 1500
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

$tester = new FPM\Tester($cfg, '<?php echo isset($_GET["big"]) ? str_repeat("x", 16 * 1024 * 1024) : "ok";');
$tester->start();
$tester->expectLogStartNotices();

$script = '/' . basename($tester->makeSourceFile());
$httpAddr = $tester->getAddr('ipv4', '[http]');
$operator = $tester->getListen('{{ADDR[operator]}}');
[$host, $port] = explode(':', $httpAddr);

function connect(string $host, string $port, int $rcvbuf = 0)
{
    $ctx = stream_context_create(['socket' => ['so_rcvbuf' => $rcvbuf ?: 87380]]);
    $fp = stream_socket_client("tcp://$host:$port", $errno, $errstr, 5, STREAM_CLIENT_CONNECT, $ctx);
    if (!$fp) {
        echo "FAIL: connect: $errstr ($errno)\n";
        exit(1);
    }
    return $fp;
}

/* One keep-alive request whose body is "ok". */
function firstRequest($fp, string $host, string $script): void
{
    fwrite($fp, "GET $script HTTP/1.1\r\nHost: $host\r\n\r\n");
    stream_set_timeout($fp, 5);
    $head = '';
    while (!str_contains($head, "\r\n\r\n")) {
        $line = fgets($fp);
        if ($line === false) {
            echo "FAIL: no response head: " . var_export($head, true) . "\n";
            exit(1);
        }
        $head .= $line;
    }
    if (!str_starts_with($head, 'HTTP/1.1 200')) {
        echo "FAIL: unexpected first response:\n$head\n";
        exit(1);
    }
    /* The upstream answers chunked: "2\r\nok\r\n0\r\n\r\n". */
    $body = '';
    while (!str_ends_with($body, "0\r\n\r\n")) {
        $line = fgets($fp);
        if ($line === false) {
            echo "FAIL: truncated body: " . var_export($body, true) . "\n";
            exit(1);
        }
        $body .= $line;
    }
    if (!str_contains($body, 'ok')) {
        echo "FAIL: body is not ok: " . var_export($body, true) . "\n";
        exit(1);
    }
}

/* Seconds until the peer closes; null when it is still open after $max. */
function waitClosed($fp, float $max, ?callable $trickle = null): ?float
{
    $start = microtime(true);
    stream_set_blocking($fp, false);
    while (microtime(true) - $start < $max) {
        $r = [$fp];
        $w = $e = null;
        if (stream_select($r, $w, $e, 0, 100000) > 0) {
            $data = @fread($fp, 8192);
            if ($data === '' || $data === false) {
                return microtime(true) - $start;
            }
        }
        if ($trickle) {
            $trickle();
        }
    }
    return null;
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

/* [idle] */
$fp = connect($host, $port);
firstRequest($fp, $host, $script);
$t = waitClosed($fp, 12);
fclose($fp);
if ($t === null) {
    echo "FAIL: [idle] idle keep-alive connection still open after 12 s\n";
    exit(1);
}
if ($t < 2.0 || $t > 6.0) {
    echo sprintf("FAIL: [idle] closed after %.1f s, want about 3 (http.keepalive_timeout = 3000)\n", $t);
    exit(1);
}
echo "idle: closed\n";

/* [second] */
$fp = connect($host, $port);
firstRequest($fp, $host, $script);
$next = "GET $script HTTP/1.1\r\nHost: $host\r\nX-Slow: 1\r\n\r\n";
$sent = 0;
$trickle = function () use (&$sent, $fp, $next): void {
    static $last = 0.0;
    if (microtime(true) - $last >= 0.2 && $sent < strlen($next) - 4) {
        if (@fwrite($fp, $next[$sent]) === 1) {
            $sent++;
        }
        $last = microtime(true);
    }
};
$t = waitClosed($fp, 12, $trickle);
fclose($fp);
if ($t === null) {
    echo "FAIL: [second] trickled second request still open after 12 s\n";
    exit(1);
}
if ($t > 2.5 || $t < 1.0) {
    echo sprintf("FAIL: [second] closed after %.1f s, want about 1.5 (http.read_timeout from the first byte)\n", $t);
    exit(1);
}
echo "second: closed\n";

/* [reader] */
if (connectionsOpen($operator) !== 0) {
    echo "FAIL: [reader] connections_open is not 0 before the slow reader\n";
    exit(1);
}
$fp = connect($host, $port, 4096);
fwrite($fp, "GET $script?big=1 HTTP/1.1\r\nHost: $host\r\n\r\n");
/* Never read. The gateway is stuck writing 16 MiB into a full socket. */
$open = 0;
for ($i = 0; $i < 20 && $open < 1; $i++) {
    usleep(100000);
    $open = connectionsOpen($operator);
}
if ($open < 1) {
    echo "FAIL: [reader] the stalled connection never showed in connections_open\n";
    exit(1);
}
$gone = false;
for ($i = 0; $i < 100; $i++) {
    usleep(100000);
    if (connectionsOpen($operator) === 0) {
        $gone = true;
        break;
    }
}
fclose($fp);
if (!$gone) {
    echo "FAIL: [reader] connections_open still above 0 after 10 s (http.write_timeout = 1500)\n";
    exit(1);
}
echo "reader: closed\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
idle: closed
second: closed
reader: closed
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
