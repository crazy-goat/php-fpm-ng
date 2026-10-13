--TEST--
fpm-ng: a truncated gateway reply logs only the body bytes the slow client actually got (issue #635)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "fpmng-raw-upstream.inc";

/* Issue #635: fpm_http_finish_truncated() shutdown()s the client socket with
 * the tail of the body still in libevent's userspace output buffer, so a slow
 * client never sees those bytes while the access log counted them as sent. The
 * gateway now subtracts the unsent buffer from bytes_out, so the log line
 * matches what left the process (the direction taken is "drop and correct the
 * count"; docs/gateway.md says so).
 *
 * A client whose receive buffer is 4 KiB and which does not read while the
 * upstream writes an 8 MiB body (of a declared 16 MiB Content-Length) keeps
 * most of that body in the gateway's output buffer (measured: a loopback sender
 * pushes about 2.6 MiB into the kernel before EAGAIN, so 8 MiB guarantees a
 * non-empty pending buffer). The upstream then closes mid-body. The test waits
 * for the access-log line (written at truncation), then drains the client and
 * compares the two numbers. */

function check(bool $condition, string $message, string $extra = ''): void
{
    if (!$condition) {
        echo "FAIL: $message\n$extra\n";
        exit(1);
    }
}

/* response_buffer = 0 keeps the gateway reading the upstream so the whole body
 * is queued for the client; the default 1M would pause the read instead. */
$raw = new FpmngRaw('trunc635', '', "http.access_log = {{FILE:LOG:ACC}}\nhttp.response_buffer = 0");
$accessLog = $raw->tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);

$bodyLen = 8 * 1024 * 1024;
$body = str_repeat('x', $bodyLen);
/* Declared larger than sent: the upstream closes mid-body, which is what makes
 * the gateway truncate the client reply instead of completing it. */
$declaredLen = 16 * 1024 * 1024;

$parts = explode(':', $raw->http);
$ctx = stream_context_create(['socket' => ['so_rcvbuf' => 4096]]);
$client = stream_socket_client("tcp://{$parts[0]}:{$parts[1]}", $errno, $errstr, 5, STREAM_CLIENT_CONNECT, $ctx);
check($client !== false, "client connect: $errstr");
stream_set_blocking($client, false);

$request = fpmng_raw_get('/evil/slow');
$deadline = microtime(true) + 5;
while ($request !== '' && microtime(true) < $deadline) {
    $n = @fwrite($client, $request);
    if ($n === false) {
        break;
    }
    $request = substr($request, $n);
}

$u = $raw->conn();
check($u !== null, 'the gateway never dialled the raw upstream');
$raw->readRequest($u);

$response = "HTTP/1.1 200 OK\r\nContent-Length: $declaredLen\r\n\r\n" . $body;
$deadline = microtime(true) + 10;
while ($response !== '' && microtime(true) < $deadline) {
    $n = @fwrite($u, $response);
    if ($n === false) {
        break;
    }
    $response = substr($response, $n);
    if ($n === 0) {
        usleep(1000);
    }
}
check($response === '', 'the raw upstream could not write its whole body');

/* Let the gateway read the body and then the FIN; the access-log line is
 * written at truncation. The client is still not reading. */
usleep(200000);
fclose($u);

$deadline = microtime(true) + 8;
$log = '';
while (microtime(true) < $deadline) {
    $log = (string) @file_get_contents($accessLog);
    if (str_contains($log, '/evil/slow')) {
        break;
    }
    usleep(50000);
}
check(str_contains($log, '/evil/slow'), 'no access-log line for the truncated request', $log);

/* Now read everything the gateway sent before it cut the connection. */
stream_set_blocking($client, true);
stream_set_timeout($client, 6);
$resp = '';
while (!feof($client)) {
    $chunk = fread($client, 65536);
    if ($chunk === false) {
        break;
    }
    if ($chunk === '') {
        if (stream_get_meta_data($client)['timed_out'] ?? false) {
            break;
        }
        continue;
    }
    $resp .= $chunk;
}
fclose($client);

$headEnd = strpos($resp, "\r\n\r\n");
check($headEnd !== false, 'no response head reached the client', substr($resp, 0, 200));
$received = strlen($resp) - $headEnd - 4;
check($received > 0, 'the client got no body at all');
check($received < $bodyLen, "the reply was not truncated: $received of $bodyLen bytes");

check(preg_match('/" \d+ (\d+) "/', $log, $m) === 1, 'cannot parse bytes_out from the access log', $log);
$logged = (int) $m[1];
check($logged === $received, "bytes_out is $logged but the client received $received");

echo "truncated: yes\n";
echo "bytes-out: matches\n";

$raw->finish();
?>
--EXPECT--
truncated: yes
bytes-out: matches
--CLEAN--
<?php
require_once "fpmng-raw-upstream.inc";
FpmngRaw::clean('trunc635');
