--TEST--
fpm-ng: the HTTP gateway bounds the whole request header block like HTTP-direct (issue #117)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #115 bounded a request header *name* on both transports, which says
 * nothing about how many of them a client may send. Both HTTP-direct
 * executors have always capped the block at FPM_HTTP_HEADERS_MAX
 * (evhttp_set_max_headers_size, fpm_http_direct.c and
 * fpm_http_direct_worker.c); the gateway never called that function, so its
 * block limit was libevent's default EV_SIZE_MAX. One gateway process serves
 * every connection of the pool, so the bytes were charged against all of them
 * at once. This test pins the bound on the gateway. */

const HEADERS_MAX = 64 * 1024;	/* FPM_HTTP_HEADERS_MAX */
/* libevent charges the request line plus every header line, each without its
 * CRLF (libevent 2.1.12-stable, http.c:2046 evhttp_parse_firstline_() and
 * http.c:2108 evhttp_parse_headers_()), so block() below builds to that rule.
 * Measured on 192.168.8.50, 2026-09-09: 65536 counted bytes are served and
 * 65537 are refused, to the byte. The test still asks a quarter of a kilobyte
 * either side rather than the exact edge, so a libevent that counted the
 * terminators (2 bytes x ~73 lines here) would not turn a correct bound into
 * a red test. */
const SLACK = 256;

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Sends a raw request and returns its status line, or '' when the gateway
 * closed without answering. The write is best-effort on purpose: the server
 * refuses an over-long block mid-stream and closes, so the tail of a request
 * this size can land on a closed socket. */
function request(string $addr, string $raw): string
{
    $fp = stream_socket_client("tcp://$addr", $errno, $error, 5);
    check($fp !== false, "connect to $addr failed: $error");
    stream_set_timeout($fp, 5);
    while ($raw !== '') {
        $written = @fwrite($fp, $raw);
        if ($written === false || $written === 0) break;
        $raw = substr($raw, $written);
    }
    $status = (string) fgets($fp);
    /* Drain, so the close is ours and not a RST that eats the status line. */
    while (!feof($fp) && stream_get_contents($fp, 8192) !== '') {
    }
    fclose($fp);
    return $status;
}

/* Builds a request whose block costs libevent exactly $bytes, counted the way
 * it counts: the request line and each `X-Pad-NNNN: <fill>` line, CRLFs
 * excluded. The name stays well inside FPM_HTTP_HEADER_NAME_MAX so this test
 * fails for one reason only. */
function block(int $bytes): string
{
    $lines = ['GET /env.php HTTP/1.1', 'Host: block.test', 'Connection: close'];
    $remaining = $bytes - array_sum(array_map('strlen', $lines));
    check($remaining > 0, "block($bytes) is smaller than the fixed lines");

    /* At most 900 bytes per line, so no single line comes near
     * FPM_HTTP_HEADER_NAME_MAX or any per-line limit; the remainder is spread
     * evenly instead of left on a short last line. */
    $count = (int) ceil($remaining / 900);
    for ($i = 0; $i < $count; $i++) {
        $len = intdiv($remaining, $count - $i);
        $name = sprintf('X-Pad-%04d: ', $i);
        $lines[] = $name . str_repeat('v', $len - strlen($name));
        $remaining -= $len;
    }
    check($remaining === 0, "block accounting is off by $remaining");
    check(array_sum(array_map('strlen', $lines)) === $bytes, 'block length mismatch');

    return implode("\r\n", $lines) . "\r\n\r\n";
}

$root = sys_get_temp_dir() . '/fpmng-gateway-header-block-' . getmypid();
@mkdir($root, 0700, true);
file_put_contents("$root/env.php", "<?php echo 'served';\n");

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[web]
listen = {{ADDR}}
chdir = $root
pm = static
pm.max_children = 1
pool.type = http
http.listen = {{ADDR[http]}}
http.front_controller = /env.php
EOT;

$tester = new FPM\Tester($cfg, file_get_contents("$root/env.php"));
$tester->start();
$tester->expectLogStartNotices();
$http = $tester->getAddr('ipv4', '[http]');

/* Served, and worth having for a second reason: a block this size does not
 * fit one FastCGI PARAMS record, so this is also the only test that exercises
 * the gateway's multi-record request head. Both halves of that path were
 * broken until #117 -- BEGIN_REQUEST was written after the flushed records,
 * and a record could be padded past what php-src accepts. */
$status = request($http, block(HEADERS_MAX - SLACK));
check(str_contains($status, ' 200 '), 'block just under the bound: ' . var_export($status, true));
echo "block-under-limit: ok\n";

/* Refused with a status, not served and not absorbed. libevent answers 400
 * (EVREQ_HTTP_INVALID_HEADER -> HTTP_BADREQUEST, http.c:664
 * evhttp_connection_incoming_fail()) -- the same status the header-name bound
 * from #115 returns, so a client sees one answer for "your headers are not
 * acceptable". */
$status = request($http, block(HEADERS_MAX + SLACK));
check(str_contains($status, ' 400 '), 'block just over the bound: ' . var_export($status, true));
echo "block-over-limit: ok\n";

/* A block within the bound still has to fit FastCGI, which cannot carry a
 * single name/value pair larger than one record. Both shapes below are inside
 * the 64 KiB block and were answered 502 -- the exact symptom this issue is
 * about -- until the gateway started refusing an oversized pair up front:
 * the header line makes a 65533-byte HTTP_* pair, and the URI a 65539-byte
 * REQUEST_URI whose length wrapped in the two-byte record header. The status
 * asserted is 400 either way: on a libevent that charged the CRLFs these
 * blocks would be over the bound instead, which is the same answer. */
$name = 'X-' . str_repeat('n', 128);
$line = "$name: " . str_repeat('v', 65520 - 130);
$status = request($http, "GET / HTTP/1.0\r\n$line\r\n\r\n");
check(str_contains($status, ' 400 '), 'one header line filling the block: ' . var_export($status, true));
echo "single-header-pair: ok\n";

$status = request($http, 'GET /' . str_repeat('u', 65523 - 5) . ".php HTTP/1.0\r\n\r\n");
check(str_contains($status, ' 400 '), 'one URI filling the block: ' . var_export($status, true));
echo "single-uri-pair: ok\n";

/* The pool still works afterwards: the refusal took the connection, not the
 * gateway process or the one worker behind it. */
$status = request($http, "GET /env.php HTTP/1.1\r\nHost: block.test\r\nConnection: close\r\n\r\n");
check(str_contains($status, ' 200 '), 'ordinary request after a refusal: ' . var_export($status, true));
echo "gateway-alive: ok\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink("$root/env.php");
@rmdir($root);

?>
Done
--EXPECT--
block-under-limit: ok
block-over-limit: ok
single-header-pair: ok
single-uri-pair: ok
gateway-alive: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
