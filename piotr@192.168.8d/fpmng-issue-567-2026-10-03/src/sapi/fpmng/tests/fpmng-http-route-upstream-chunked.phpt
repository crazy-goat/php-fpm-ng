--TEST--
fpm-ng: the HTTP client transport rejects a malformed chunked upstream body audibly and reads a split trailer whole (issue #462)
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

$raw = new FpmngRaw('chunked');
$protoError = "/WARNING: \[pool gw\] http: upstream '[^']*chunked\.sock': Protocol error/";

/* ---- a chunk-size line that is not hex: "zzz" used to read as the zero chunk,
 * so a truncated body was relayed as complete and nothing was logged -------- */
$from = strlen($raw->log());
$raw->send(fpmng_raw_get('/evil/a'));
$u = $raw->conn();
check($u !== null, 'the gateway never dialled the raw upstream', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n");
usleep(100000);
fwrite($u, "zzz\r\n");
$raw->readResponse();
check($raw->waitLog($protoError, $from) !== null, 'garbage chunk-size line: no log line', $raw);
check($raw->closedByGateway($u), 'garbage chunk-size line: the upstream connection was kept', $raw);
echo "garbage-chunk-size: rejected and logged\n";

/* ---- more than 16 hex digits: remaining*16+d wrapped size_t, the parser ate
 * 12 payload bytes and took the real framing for body data ----------------- */
$from = strlen($raw->log());
$raw->send(fpmng_raw_get('/evil/b'), true);
$u = $raw->conn();
check($u !== null, 'no second upstream connection', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
usleep(100000);
fwrite($u, "12345678901234567890\r\nAAAAAAAAAA\r\n");
usleep(100000);
@fwrite($u, "0\r\n\r\n");
$resp = $raw->readResponse();
check(!str_contains($resp, 'AAAAAAAAAA'), 'oversized chunk-size: the payload was relayed as a body', $raw, $resp);
check($raw->waitLog($protoError, $from) !== null, 'oversized chunk-size line: no log line', $raw);
echo "oversized-chunk-size: rejected and logged\n";

/* ---- control: extensions, upper-case hex and blanks stay legal ------------ */
$from = strlen($raw->log());
$raw->send(fpmng_raw_get('/evil/c'), true);
$u = $raw->conn();
check($u !== null, 'no third upstream connection', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nA;ext=1\r\n0123456789\r\n0\r\n\r\n");
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 200') && str_contains($resp, '0123456789'), 'valid chunked body with an extension was refused', $raw, $resp);
check(!preg_match($protoError, substr($raw->log(), $from)), 'valid chunked body logged a protocol error', $raw);
echo "valid-chunked: relayed\n";

/* ---- a trailer line split across two writes ("x: y\r" | "\n"): the lone "\n"
 * used to be taken for the blank line, ending response A early and leaving the
 * real terminator on the connection. Reuse proves A ended at the real one. -- */
$raw->send(fpmng_raw_get('/evil/d'), true);
$u = $raw->conn();
check($u !== null, 'no fourth upstream connection', $raw);
$accepts = $raw->accepts;
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\nX-T: y\r");
usleep(300000);
fwrite($u, "\n");
usleep(300000);
fwrite($u, "\r\n");
$resp = $raw->readResponse();
check(str_contains($resp, 'hello'), 'split trailer: response A not relayed', $raw, $resp);
usleep(300000);
$raw->send(fpmng_raw_get('/evil/e'));
check($raw->accept(1.0) === null, 'split trailer: the upstream connection was torn down instead of reused', $raw);
$req = $raw->readRequest($u);
check(str_contains($req, '/evil/e'), 'split trailer: request B never arrived on the reused connection', $raw, $req);
fwrite($u, "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nresp-B");
$resp = $raw->readResponse();
check(str_ends_with($resp, 'resp-B'), 'split trailer: request B got a foreign response', $raw, $resp);
echo "split-trailer: one response, connection reused\n";

$raw->finish();
?>
--EXPECT--
garbage-chunk-size: rejected and logged
oversized-chunk-size: rejected and logged
valid-chunked: relayed
split-trailer: one response, connection reused
--CLEAN--
<?php
require_once "fpmng-raw-upstream.inc";
FpmngRaw::clean('chunked');
?>
