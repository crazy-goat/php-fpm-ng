--TEST--
fpm-ng: a gateway answers 502 when the upstream sends a valid and an invalid "Status:" line in either order (issue #837)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #837: fpm_http_start_reply() walks every "Status:" line of the CGI
 * header block. A later valid line used to clear the invalid_status flag that
 * an earlier invalid line had set, so "Status: abc" followed by "Status: 404
 * Valid" went out as "HTTP/1.1 404 Valid" with the upstream body, while the
 * WARNING still promised a 502. The raw FastCGI upstream below sends both
 * orders; the reverse order was already a 502 and must stay one. */

function check(bool $condition, string $message, string $extra = ''): void
{
    if (!$condition) {
        echo "FAIL: $message\n$extra\n--- log ---\n" . log_tail() . "\n";
        exit(1);
    }
}

/* A FastCGI record: 8-byte header, content, padding to a multiple of 8.
 * Request id 1, which is what the gateway sends. */
function fcgi_record(int $type, string $content): string
{
    $pad = (8 - strlen($content) % 8) % 8;

    return pack('CCnnCx', 1, $type, 1, strlen($content), $pad) . $content . str_repeat("\0", $pad);
}

const FCGI_STDOUT = 6;
const FCGI_END_REQUEST = 3;
/* FCGI_END_REQUEST content: appStatus (4, network order), protocolStatus (1),
 * reserved (3). Status 0 is FCGI_REQUEST_COMPLETE -- a clean end of request. */
function fcgi_end(): string
{
    return pack('NCxxx', 0, 0);
}

/* --- state: one client connection, one raw upstream ------------------- */
$GLOBALS['client'] = null;
$GLOBALS['client_buf'] = '';
$GLOBALS['http'] = '';
$GLOBALS['server'] = null;
$GLOBALS['last_upstream'] = null;

function client_open(bool $fresh): void
{
    if ($fresh && $GLOBALS['client']) {
        fclose($GLOBALS['client']);
        $GLOBALS['client'] = null;
        $GLOBALS['client_buf'] = '';
    }
    if (!$GLOBALS['client']) {
        $sock = stream_socket_client('tcp://' . $GLOBALS['http'], $errno, $errstr, 5);
        if (!$sock) {
            echo "FAIL: cannot connect to the gateway: $errstr\n";
            exit(1);
        }
        stream_set_blocking($sock, false);
        $GLOBALS['client'] = $sock;
    }
}

function client_send(string $request): void
{
    $sock = $GLOBALS['client'];
    $deadline = microtime(true) + 5;
    while ($request !== '' && microtime(true) < $deadline) {
        $n = @fwrite($sock, $request);
        if ($n === false) {
            break;
        }
        $request = substr($request, $n);
        if ($n === 0) {
            $r = null;
            $w = [$sock];
            $e = null;
            stream_select($r, $w, $e, 0, 100000);
        }
    }
}

/** Length of the first complete response in $b, or null when more bytes are needed. */
function framed_length(string $b): ?int
{
    $p = strpos($b, "\r\n\r\n");
    if ($p === false) {
        return null;
    }
    $head = substr($b, 0, $p);
    $off = $p + 4;
    if (preg_match('/^Transfer-Encoding:[^\r\n]*chunked/mi', $head)) {
        while (true) {
            $e = strpos($b, "\r\n", $off);
            if ($e === false) {
                return null;
            }
            $n = (int) hexdec(substr($b, $off, $e - $off));
            if ($n === 0) {
                return strlen($b) >= $e + 4 ? $e + 4 : null;
            }
            $off = $e + 2 + $n + 2;
            if ($off > strlen($b)) {
                return null;
            }
        }
    }
    if (preg_match('/^Content-Length:\s*(\d+)/mi', $head, $m)) {
        return strlen($b) >= $off + (int) $m[1] ? $off + (int) $m[1] : null;
    }

    return null;  /* close-delimited */
}

/** The next complete response on the client connection. */
function client_read(float $seconds = 6.0): string
{
    $sock = $GLOBALS['client'];
    $deadline = microtime(true) + $seconds;
    while (microtime(true) < $deadline) {
        $len = framed_length($GLOBALS['client_buf']);
        if ($len !== null) {
            $out = substr($GLOBALS['client_buf'], 0, $len);
            $GLOBALS['client_buf'] = substr($GLOBALS['client_buf'], $len);

            return $out;
        }
        $chunk = fread($sock, 65536);
        if ($chunk !== '' && $chunk !== false) {
            $GLOBALS['client_buf'] .= $chunk;
            continue;
        }
        if (feof($sock)) {
            break;
        }
        usleep(20000);
    }
    $out = $GLOBALS['client_buf'];
    $GLOBALS['client_buf'] = '';

    return $out;
}

function status_line(string $response): string
{
    return strtok($response, "\r\n") ?: '(nothing)';
}

/* --- the raw upstream: whatever the test writes is the whole reply ---- */
function accept_upstream(float $seconds = 5.0)
{
    $r = [$GLOBALS['server']];
    $w = $e = null;
    if (stream_select($r, $w, $e, (int) $seconds, (int) (($seconds - (int) $seconds) * 1e6)) < 1) {
        return null;
    }
    $conn = @stream_socket_accept($GLOBALS['server'], 0);
    if ($conn) {
        stream_set_blocking($conn, false);
        $GLOBALS['last_upstream'] = $conn;
    }

    return $conn;
}

/** The connection this request travels on: a fresh one, or the keep-alive one the gateway kept. */
function upstream(float $dial = 5.0)
{
    return accept_upstream($dial) ?? $GLOBALS['last_upstream'];
}

/** Reads the request head the gateway wrote, so the reply cannot race it out. */
function upstream_read($conn, float $seconds = 3.0): string
{
    $out = '';
    $deadline = microtime(true) + $seconds;
    stream_set_blocking($conn, false);
    while (microtime(true) < $deadline) {
        $chunk = fread($conn, 65536);
        if ($chunk !== '' && $chunk !== false) {
            $out .= $chunk;
            if (str_contains($out, 'REQUEST_METHOD') && str_ends_with($out, "\x01\x05\x00\x01\x00\x00\x00\x00")) {
                break;
            }
            continue;
        }
        if (feof($conn)) {
            break;
        }
        usleep(20000);
    }
    stream_set_blocking($conn, true);

    return $out;
}

function log_tail(): string
{
    return (string) @file_get_contents($GLOBALS['error_log']);
}

function wait_log(string $pattern, int $from = 0, float $seconds = 8.0): ?string
{
    $deadline = microtime(true) + $seconds;
    do {
        $tail = substr(log_tail(), $from);
        if (preg_match($pattern, $tail)) {
            return $tail;
        }
        usleep(50000);
    } while (microtime(true) < $deadline);

    return null;
}

function wait_access(string $pattern, float $seconds = 8.0): ?string
{
    $deadline = microtime(true) + $seconds;
    do {
        $acc = (string) @file_get_contents($GLOBALS['access_log']);
        if (preg_match($pattern, $acc)) {
            return $acc;
        }
        usleep(50000);
    } while (microtime(true) < $deadline);

    return null;
}

/* --- configuration: a gateway in front of a FastCGI target ----------- */
$root = sys_get_temp_dir() . '/fpmng-gateway-status-duplicate';
$sock = $root . '.sock';
@mkdir($root, 0700, true);
@unlink($sock);

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $root
http.gateways = 1
http.access_log = {{FILE:LOG:ACC}}
http.route[app] = /
[app]
pool.type = fastcgi
listen = $sock
chdir = $root
pm = static
pm.max_children = 1
EOT;

$tester = new FPM\Tester($cfg);
/* A file, not the -O stderr pipe: both the WARNING and the access-log status
 * are read back below. */
@unlink($tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR));
$tester->start([], false);
$tester->switchLogSource('{{FILE:LOG}}');
$tester->expectLogStartNotices();
$GLOBALS['error_log'] = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ERR);
$GLOBALS['access_log'] = $tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);
$GLOBALS['http'] = $tester->getAddr('ipv4', '[http]');

/* Take over the target's address: from here on the "worker pool" is a script. */
@unlink($sock);
$server = @stream_socket_server('unix://' . $sock, $errno, $errstr);
if (!$server) {
    echo "FAIL: raw upstream bind failed: $errstr\n";
    exit(1);
}
$GLOBALS['server'] = $server;

/* ---- 1: invalid, then valid: the 502 stands ----------------------------
 * The valid line must not undo the rejection, and the upstream's headers and
 * body must not reach the client. */
$from = strlen(log_tail());
client_open(true);
client_send("GET /a.php HTTP/1.1\r\nHost: t\r\n\r\n");
$u = upstream();
check($u !== null, 'the gateway never dialled the raw upstream');
upstream_read($u);
fwrite($u, fcgi_record(FCGI_STDOUT, "Status: abc\r\nStatus: 404 Valid\r\nX-Upstream: leaked\r\n\r\nhello")
    . fcgi_record(FCGI_END_REQUEST, fcgi_end()));
$resp = client_read();
check(status_line($resp) === 'HTTP/1.1 502 Bad Gateway', 'invalid then valid: not a 502', $resp);
check(str_contains($resp, 'X-Upstream') === false, 'invalid then valid: upstream header forwarded', $resp);
check(str_contains($resp, 'hello') === false, 'invalid then valid: upstream body forwarded', $resp);
check(wait_log("/upstream sent invalid Status 'abc', answering 502/", $from) !== null,
    'invalid then valid: no WARNING for abc');
check(wait_access('/"GET \/a\.php HTTP\/1\.1" 502 /') !== null,
    'invalid then valid: the access log does not say 502');
echo "invalid-then-valid: 502\n";

/* ---- 2: valid, then invalid: still a 502 ------------------------------- */
$from = strlen(log_tail());
client_open(true);
client_send("GET /b.php HTTP/1.1\r\nHost: t\r\n\r\n");
$u = upstream(0.5);
check($u !== null, 'no second upstream connection');
upstream_read($u);
fwrite($u, fcgi_record(FCGI_STDOUT, "Status: 404 Valid\r\nStatus: abc\r\nX-Upstream: leaked\r\n\r\nhello")
    . fcgi_record(FCGI_END_REQUEST, fcgi_end()));
$resp = client_read();
check(status_line($resp) === 'HTTP/1.1 502 Bad Gateway', 'valid then invalid: not a 502', $resp);
check(str_contains($resp, 'X-Upstream') === false, 'valid then invalid: upstream header forwarded', $resp);
check(str_contains($resp, 'hello') === false, 'valid then invalid: upstream body forwarded', $resp);
check(wait_log("/upstream sent invalid Status 'abc', answering 502/", $from) !== null,
    'valid then invalid: no WARNING for abc');
check(wait_access('/"GET \/b\.php HTTP\/1\.1" 502 /') !== null,
    'valid then invalid: the access log does not say 502');
echo "valid-then-invalid: 502\n";

/* ---- 3: a valid block with no invalid line is still served ------------- */
$from = strlen(log_tail());
client_open(true);
client_send("GET /c.php HTTP/1.1\r\nHost: t\r\n\r\n");
$u = upstream(0.5);
check($u !== null, 'the gateway dialled nothing for the third request');
upstream_read($u);
fwrite($u, fcgi_record(FCGI_STDOUT, "Status: 404 Gone\r\nContent-Type: text/plain\r\n\r\nhello")
    . fcgi_record(FCGI_END_REQUEST, fcgi_end()));
$resp = client_read();
check(status_line($resp) === 'HTTP/1.1 404 Gone', 'valid only: wrong status', $resp);
check(str_contains($resp, 'Content-Type: text/plain'), 'valid only: header not forwarded', $resp);
check(str_contains(substr(log_tail(), $from), 'invalid Status') === false,
    'valid only: reported as invalid', log_tail());
echo "valid-only: served\n";

fclose($server);
$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink($sock);
@rmdir($root);

?>
Done
--EXPECT--
invalid-then-valid: 502
valid-then-invalid: 502
valid-only: served
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();

$root = sys_get_temp_dir() . '/fpmng-gateway-status-duplicate';
@unlink("$root.sock");
@rmdir($root);
?>
