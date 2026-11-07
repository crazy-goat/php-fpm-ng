--TEST--
fpm-ng: http.proxy_protocol reads a PROXY v1/v2 header from trusted sources and closes every other connection (issue #651)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #651: with http.proxy_protocol on, the gateway reads a PROXY header
 * before any HTTP byte. A header from a trusted source (127.0.0.1 here) sets
 * the client address that REMOTE_ADDR and the access log use; LOCAL keeps the
 * socket peer. A source that is not trusted, a request without a header, and
 * a malformed header all get the connection closed, and the target is not
 * reached. The untrusted source is 127.0.0.2, bound on the client side. */
$docroot = sys_get_temp_dir() . '/fpmng-proxy-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', <<<'PHP'
<?php
echo 'remote=' . ($_SERVER['REMOTE_ADDR'] ?? 'none') . "\n";
PHP);

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
http.proxy_protocol = yes
http.proxy_protocol_trusted = 127.0.0.1
http.route[web] = /
[web]
pool.type = fastcgi
listen = {{ADDR}}
chdir = $docroot
pm = static
pm.max_children = 2
EOT;

const GET = "GET /index.php HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";

/* One connection: sends $bytes, returns everything the gateway answers before
 * it closes. An empty string means the connection was closed unanswered. */
function exchange(string $addr, string $bytes, string $bindto = ''): string
{
    $ctx = stream_context_create(['socket' => $bindto === '' ? [] : ['bindto' => $bindto]]);
    $fp = @stream_socket_client("tcp://$addr", $errno, $errstr, 5, STREAM_CLIENT_CONNECT, $ctx);
    if ($fp === false) {
        return "CONNECT FAILED: $errstr";
    }
    stream_set_timeout($fp, 5);
    fwrite($fp, $bytes);
    $resp = '';
    while (!feof($fp)) {
        $chunk = fread($fp, 8192);
        if ($chunk === false || $chunk === '') {
            break;
        }
        $resp .= $chunk;
    }
    fclose($fp);
    return $resp;
}

function remoteOf(string $resp): ?string
{
    return preg_match('/remote=(\S+)/', $resp, $m) === 1 ? $m[1] : null;
}

function check(bool $ok, string $what): void
{
    if (!$ok) {
        echo "FAIL: $what\n";
        exit(1);
    }
}

/* PROXY v2 binary header, INET, PROXY command: 192.0.2.20:40000 -> 198.51.100.1:443. */
$v2inet = "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A\x21\x11\x00\x0C"
    . pack('N', ip2long('192.0.2.20')) . pack('N', ip2long('198.51.100.1'))
    . pack('n', 40000) . pack('n', 443);
$v2local = "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A\x20\x00\x00\x00";
/* The same v2 INET header with the transport nibble set to DGRAM (2): the listener
 * is TCP, so the header is refused (issue #651, review R3). */
$v2dgram = $v2inet;
$v2dgram[13] = "\x12";

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    $addr = $tester->getAddr('ipv4', '[http]');

    /* v1 and GET in one segment: the header is consumed, the request is served. */
    $r = exchange($addr, "PROXY TCP4 192.0.2.10 198.51.100.1 56324 443\r\n" . GET);
    check(remoteOf($r) === '192.0.2.10', "v1 TCP4: REMOTE_ADDR is not the header source: $r");
    echo "v1 TCP4 from a trusted source: ok\n";

    /* v2 INET in one segment. */
    $r = exchange($addr, $v2inet . GET);
    check(remoteOf($r) === '192.0.2.20', "v2 INET: REMOTE_ADDR is not the header source: $r");
    echo "v2 INET from a trusted source: ok\n";

    /* v2 LOCAL: no client address, the socket peer stays. */
    $r = exchange($addr, $v2local . GET);
    check(remoteOf($r) === '127.0.0.1', "v2 LOCAL: REMOTE_ADDR is not the socket peer: $r");
    echo "v2 LOCAL keeps the socket peer: ok\n";

    /* Untrusted source with a valid header: closed, target not reached. */
    $r = exchange($addr, "PROXY TCP4 192.0.2.10 198.51.100.1 56324 443\r\n" . GET, '127.0.0.2:0');
    check($r === '' || remoteOf($r) === null, "untrusted source got an answer: $r");
    check(!str_contains($r, 'remote='), "untrusted source reached the target: $r");
    echo "untrusted source is closed: ok\n";

    /* Plain request, no header: closed, target not reached. */
    $r = exchange($addr, GET);
    check(!str_contains($r, 'remote='), "request without a header reached the target: $r");
    echo "request without a header is closed: ok\n";

    /* A header that trickles in and never completes: the read deadline
     * (http.read_timeout, default 5 s) closes the connection with no answer. */
    $started = microtime(true);
    $r = exchange($addr, "PROXY TC");
    check(!str_contains($r, 'remote='), "partial header reached the target: $r");
    check(microtime(true) - $started < 12, "partial header held the connection past the read deadline");
    echo "partial header is closed at the read deadline: ok\n";

    /* v2 with a DGRAM transport from a trusted source: closed, target not reached. */
    $r = exchange($addr, $v2dgram . GET);
    check(!str_contains($r, 'remote='), "v2 DGRAM transport reached the target: $r");
    echo "v2 DGRAM transport is closed: ok\n";

    /* Malformed v1 (bare LF instead of CRLF): closed, target not reached. */
    $r = exchange($addr, "PROXY TCP4 192.0.2.10 198.51.100.1 56324 443\n" . GET);
    check(!str_contains($r, 'remote='), "malformed header reached the target: $r");
    echo "malformed header is closed: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
}

@unlink("$docroot/index.php");
@rmdir($docroot);
echo "Done\n";
?>
--EXPECT--
v1 TCP4 from a trusted source: ok
v2 INET from a trusted source: ok
v2 LOCAL keeps the socket peer: ok
untrusted source is closed: ok
request without a header is closed: ok
partial header is closed at the read deadline: ok
v2 DGRAM transport is closed: ok
malformed header is closed: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
