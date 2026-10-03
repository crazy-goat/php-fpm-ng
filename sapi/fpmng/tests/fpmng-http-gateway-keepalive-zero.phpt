--TEST--
FPM http gateway: http.keepalive_timeout = 0 keeps idle unlimited but a later request is still bounded by http.read_timeout (issue #593)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Review round 1: with keep-alive 0 the later-request read limit was never armed.
 * Idle stays unlimited (the connection must survive 3.5 s of silence, longer than
 * the 1500 ms read_timeout), then a second request trickled one byte at a time
 * must be cut about http.read_timeout after its first byte. */

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
http.keepalive_timeout = 0
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR[app]}}
pm = static
pm.max_children = 2
chdir = __DIR__
EOT;
$cfg = str_replace('__DIR__', __DIR__, $cfg);

$tester = new FPM\Tester($cfg, '<?php echo "ok";');
$tester->start();
$tester->expectLogStartNotices();

$script = '/' . basename($tester->makeSourceFile());
$httpAddr = $tester->getAddr('ipv4', '[http]');
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


$fp = connect($host, $port);
firstRequest($fp, $host, $script);
$t = waitClosed($fp, 3.5);
if ($t !== null) {
    echo sprintf("FAIL: idle connection closed after %.1f s with http.keepalive_timeout = 0\n", $t);
    exit(1);
}
echo "idle: open\n";

stream_set_blocking($fp, false);
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
    echo "FAIL: trickled second request still open after 12 s\n";
    exit(1);
}
if ($t > 2.5 || $t < 1.0) {
    echo sprintf("FAIL: closed after %.1f s, want about 1.5 (http.read_timeout from the first byte)\n", $t);
    exit(1);
}
echo "second: closed\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
idle: open
second: closed
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
