--TEST--
fpm-ng: the HTTP client transport never forwards a Content-Length that contradicts the framing it relayed (issue #462)
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

$raw = new FpmngRaw('length');

/* ---- Transfer-Encoding: chunked + Content-Length: 999 over a 5-byte body:
 * the verbatim 999 reached the client, whose next response was then read as
 * the missing 994 bytes of this one ---------------------------------------- */
$raw->send(fpmng_raw_get('/evil/a'));
$u = $raw->conn();
check($u !== null, 'the gateway never dialled the raw upstream', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 999\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
$resp = $raw->readResponse();
check(str_contains($resp, 'hello'), 'TE+CL: body not relayed', $raw, $resp);
check(!preg_match('/^Content-Length:\s*999/mi', FpmngRaw::head($resp)), 'TE+CL: the upstream Content-Length reached the client', $raw, $resp);
$raw->send(fpmng_raw_get('/evil/b'));
$req = $raw->readRequest($u);
check(str_contains($req, '/evil/b'), 'TE+CL: request B did not reuse the connection', $raw, $req);
fwrite($u, "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nresp-B");
$resp = $raw->readResponse();
check(str_ends_with($resp, 'resp-B'), 'TE+CL: request B did not get its own response', $raw, $resp);
echo "te-and-cl: Content-Length dropped\n";

/* ---- two different Content-Length values: refused, not "last one wins" ---- */
$from = strlen($raw->log());
$raw->send(fpmng_raw_get('/evil/c'), true);
$u = $raw->conn();
check($u !== null, 'no second upstream connection', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 10\r\n\r\n0123456789");
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 502'), 'conflicting Content-Length: not a 502', $raw, $resp);
check($raw->waitLog("/http: upstream '[^']*length\.sock' sent an invalid or conflicting Content-Length/", $from) !== null,
    'conflicting Content-Length: no log line', $raw);
echo "conflicting-cl: 502 and logged\n";

/* ---- a Content-Length that is not a plain number ------------------------- */
$from = strlen($raw->log());
$raw->send(fpmng_raw_get('/evil/d'), true);
$u = $raw->conn();
check($u !== null, 'no third upstream connection', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nContent-Length: 5abc\r\n\r\nhello");
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 502'), 'malformed Content-Length: not a 502', $raw, $resp);
check($raw->waitLog('/sent an invalid or conflicting Content-Length/', $from) !== null, 'malformed Content-Length: no log line', $raw);
echo "malformed-cl: 502 and logged\n";

/* ---- the same value twice is harmless, and forwarded once ----------------- */
$raw->send(fpmng_raw_get('/evil/e'), true);
$u = $raw->conn();
check($u !== null, 'no fourth upstream connection', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello");
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 200') && str_ends_with($resp, 'hello'), 'identical duplicate Content-Length was refused', $raw, $resp);
check(preg_match_all('/^Content-Length:/mi', FpmngRaw::head($resp)) === 1, 'identical duplicate Content-Length was forwarded twice', $raw, $resp);
echo "identical-cl: forwarded once\n";

$raw->finish();
?>
--EXPECT--
te-and-cl: Content-Length dropped
conflicting-cl: 502 and logged
malformed-cl: 502 and logged
identical-cl: forwarded once
--CLEAN--
<?php
require_once "fpmng-raw-upstream.inc";
FpmngRaw::clean('length');
?>
