--TEST--
fpm-ng: an upstream that dies after the response head ends the client reply as truncated, not as complete (issue #533)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "fpmng-raw-upstream.inc";

function check(bool $condition, string $message, FpmngRaw $raw, string $extra = ''): void
{
    if (!$condition) {
        echo "FAIL: $message\n$extra\n--- log ---\n" . $raw->log() . "\n";
        exit(1);
    }
}

$raw = new FpmngRaw('middie');

/* ---- chunked upstream body, connection closed before the 0-chunk: the client
 * used to get the gateway's own "0\r\n\r\n" and a perfectly complete reply --- */
$from = strlen($raw->log());
$raw->send(fpmng_raw_get('/evil/a'));
$u = $raw->conn();
check($u !== null, 'the gateway never dialled the raw upstream', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n");
usleep(150000);
fclose($u);
[$resp, $closed] = $raw->readUntilClose();
check(str_contains($resp, 'hello'), 'chunked: the body already received was not relayed', $raw, $resp);
check($closed, 'chunked: the client connection was kept open', $raw, $resp);
check(!str_contains($resp, "0\r\n\r\n"), 'chunked: the reply was terminated as if complete', $raw, $resp);
check($raw->waitLog("/failed after the response head was sent/", $from) !== null, 'chunked: no log line', $raw);
echo "chunked: truncated\n";

/* ---- Content-Length body cut short: the connection must not be left open for
 * the client to wait on the missing bytes ---------------------------------- */
$from = strlen($raw->log());
$raw->send(fpmng_raw_get('/evil/b'), true);
$u = $raw->conn();
check($u !== null, 'no second upstream connection', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello");
usleep(150000);
fclose($u);
[$resp, $closed] = $raw->readUntilClose();
check($closed, 'content-length: the client connection was kept open', $raw, $resp);
check(str_ends_with($resp, 'hello'), 'content-length: unexpected bytes after the partial body', $raw, $resp);
echo "content-length: truncated\n";

/* ---- control: a close-delimited body ends at the close, and that is a complete
 * reply (the gateway re-frames it as chunked) ------------------------------- */
$from = strlen($raw->log());
$raw->send(fpmng_raw_get('/evil/c'), true);
$u = $raw->conn();
check($u !== null, 'no third upstream connection', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhello");
usleep(150000);
fclose($u);
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 200') && str_contains($resp, 'hello') && str_ends_with($resp, "0\r\n\r\n"),
    'close-delimited: not a complete reply', $raw, $resp);
check(strpos($raw->log(), 'failed after the response head was sent', $from) === false, 'close-delimited: reported as a failure', $raw);
echo "close-delimited: complete\n";

/* ---- the gateway is still serving ---------------------------------------- */
$raw->send(fpmng_raw_get('/evil/d'), true);
$u = $raw->conn();
check($u !== null, 'no fourth upstream connection', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
$resp = $raw->readResponse();
check(str_ends_with($resp, 'ok'), 'the gateway stopped serving after the aborted replies', $raw, $resp);
echo "still serving\n";

$raw->finish();
?>
--EXPECT--
chunked: truncated
content-length: truncated
close-delimited: complete
still serving
--CLEAN--
<?php
require_once "fpmng-raw-upstream.inc";
FpmngRaw::clean('middie');
?>
