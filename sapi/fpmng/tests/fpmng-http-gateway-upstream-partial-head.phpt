--TEST--
fpm-ng: a FastCGI upstream that stops before the blank line closing the CGI header block is answered 502, not a complete empty reply (issue #636)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php
require_once "tester.inc";

/* Issue #636: fpm_http_finish()'s `else if (c->cgi_headers.s)` branch shipped
 * whatever the upstream had managed to say as if it were a whole reply:
 * fpm_http_start_reply(c, 0, 0) with a head length of zero parses no header
 * line at all (the walk in fpm_http_start_reply() runs from `line` to
 * `line + head_len`, so an empty range leaves code at its HTTP_OK initial
 * value) and evhttp_send_reply_end() then completes the message. The client
 * got "HTTP/1.1 200 OK" with a zero-length body -- the "truncation looks
 * complete" problem #533 fixed for a body, in the head.
 *
 * The upstream here is a script on the raw socket of the FastCGI target, so it
 * can end its reply anywhere: the gateway dials that address itself on every
 * connect, and once fpm-ng is up the test takes the address over -- the same
 * trick fpmng-raw-upstream.inc plays for an http-direct target, which needs it
 * because its class can only speak HTTP/1.1. */

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
$GLOBALS['accepts'] = 0;

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

/** The response body, de-chunked when the response was chunked. */
function body(string $response): string
{
    $p = strpos($response, "\r\n\r\n");
    if ($p === false) {
        return '';
    }
    $rest = substr($response, $p + 4);
    if (!preg_match('/^Transfer-Encoding:[^\r\n]*chunked/mi', substr($response, 0, $p))) {
        return $rest;
    }
    $out = '';
    while (($e = strpos($rest, "\r\n")) !== false && ($n = (int) hexdec(substr($rest, 0, $e))) > 0) {
        $out .= substr($rest, $e + 2, $n);
        $rest = substr($rest, $e + 2 + $n + 2);
    }

    return $out;
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
        $GLOBALS['accepts']++;
    }

    return $conn;
}

/**
 * The connection this request travels on: a fresh one, or the keep-alive one
 * the gateway kept. $dial is how long to wait for a fresh one -- a short value
 * is how a case asserts the connection was REUSED rather than dialled again.
 */
function upstream(float $dial = 5.0)
{
    return accept_upstream($dial) ?? $GLOBALS['last_upstream'];
}

/**
 * Reads the request head the gateway wrote, so the reply cannot race it out.
 * Ends on the empty FCGI_STDIN record (type 5, request id 1, no content), which
 * is the last record of a request without a body.
 */
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

/* --- configuration: a gateway in front of a FastCGI target -----------
 * A fixed name, not one derived from getmypid(): --CLEAN-- runs in another
 * process and could not find a pid-suffixed directory to remove. No scripts are
 * written into the docroot and none are needed: a .php path is never served from
 * disk (fpm_http_static.c), it always goes to the FastCGI target -- which below
 * is this script. */
$root = sys_get_temp_dir() . '/fpmng-gateway-partial-head';
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

/* A header block the gateway will buffer but not complete: two complete
 * lines, and no blank line to close them. */
$partial = "Status: 200 OK\r\nContent-Type: text/plain\r\n";

/* ---- 1: the upstream closes the connection mid-header ---------------- */
$from = strlen(log_tail());
client_open(true);
client_send("GET /a.php HTTP/1.1\r\nHost: t\r\n\r\n");
$u = upstream();
check($u !== null, 'the gateway never dialled the raw upstream');
check(str_contains(upstream_read($u), 'REQUEST_METHOD'), 'the raw upstream got no FastCGI request head');
fwrite($u, fcgi_record(FCGI_STDOUT, $partial));
/* Keeps the close a beat behind the header block: the two must land in separate
 * turns of the gateway's event loop for this case to be the one it claims to be
 * (a buffered head, then EOF) rather than one that never buffered anything. */
usleep(150000);
fclose($u);
$resp = client_read();
check(status_line($resp) === 'HTTP/1.1 502 Bad Gateway', 'closed mid-header: not a 502', $resp);
check(str_contains($resp, 'Bad Gateway'), 'closed mid-header: no error body', $resp);
check(str_contains($resp, 'Content-Type: text/plain') === false,
    'closed mid-header: the incomplete header block was forwarded', $resp);
check(wait_log('/upstream .* ended its reply after \d+ bytes of an unterminated CGI header block/', $from) !== null,
    'closed mid-header: no WARNING naming the truncation');
check(wait_access('/"GET \/a\.php HTTP\/1\.1" 502 /') !== null,
    'closed mid-header: the access log does not say 502');
echo "closed-mid-header: 502\n";

/* ---- 2: the upstream ends the request cleanly, header block unfinished --
 * END_REQUEST with no error status: a well-behaved FastCGI application that
 * simply never closed its header block. Same answer, because the same branch
 * of fpm_http_finish() sees it (fpm_http_request_done(), not a failure). */
$from = strlen(log_tail());
client_open(true);
client_send("GET /b.php HTTP/1.1\r\nHost: t\r\n\r\n");
$u = upstream();
check($u !== null, 'no second upstream connection');
upstream_read($u);
fwrite($u, fcgi_record(FCGI_STDOUT, $partial) . fcgi_record(FCGI_END_REQUEST, fcgi_end()));
$resp = client_read();
check(status_line($resp) === 'HTTP/1.1 502 Bad Gateway', 'END_REQUEST mid-header: not a 502', $resp);
check(wait_log('/upstream .* ended its reply after \d+ bytes of an unterminated CGI header block/', $from) !== null,
    'END_REQUEST mid-header: no WARNING naming the truncation');
check(wait_access('/"GET \/b\.php HTTP\/1\.1" 502 /') !== null,
    'END_REQUEST mid-header: the access log does not say 502');
echo "end-request-mid-header: 502\n";

/* ---- 3: a complete header block is still served -------------------------
 * The upstream connection of case 2 is reused (no new accept), which is what
 * says the 502 above did not cost the pool a worker slot: the request ended
 * cleanly there, so the connection went back to the pool. The client gets a
 * new connection because evhttp_send_error() closes it -- the same as for the
 * no-answer 502 in fpm_http_finish(), and the reason the reply is a reply and
 * not a reseted socket (#533's shape was the alternative and it is not used). */
$from = strlen(log_tail());
$accepts = $GLOBALS['accepts'];
client_open(true);
client_send("GET /c.php HTTP/1.1\r\nHost: t\r\n\r\n");
$u = upstream(1.0);
check($u !== null, 'the gateway dialled nothing for the third request');
check($GLOBALS['accepts'] === $accepts, 'the upstream connection of case 2 was not reused',
    'accepts: ' . $accepts . ' -> ' . $GLOBALS['accepts']);
upstream_read($u);
fwrite($u, fcgi_record(FCGI_STDOUT, "Status: 404 Gone\r\nContent-Type: text/plain\r\n\r\nhello")
    . fcgi_record(FCGI_END_REQUEST, fcgi_end()));
$resp = client_read();
check(status_line($resp) === 'HTTP/1.1 404 Gone', 'complete header block: wrong status', $resp);
check(str_contains($resp, 'Content-Type: text/plain'), 'complete header block: header not forwarded', $resp);
check(body($resp) === 'hello', 'complete header block: body not relayed', $resp);
/* No wait: the WARNING, if there were one, is written before the reply goes
 * out, and the reply is what client_read() has just returned. */
check(str_contains(substr(log_tail(), $from), 'unterminated CGI header block') === false,
    'complete header block: reported as a truncation', log_tail());
echo "complete-header-block: served\n";

fclose($server);
$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

@unlink($sock);
@rmdir($root);

?>
Done
--EXPECT--
closed-mid-header: 502
end-request-mid-header: 502
complete-header-block: served
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();

$root = sys_get_temp_dir() . '/fpmng-gateway-partial-head';
@unlink("$root.sock");
@rmdir($root);
?>
