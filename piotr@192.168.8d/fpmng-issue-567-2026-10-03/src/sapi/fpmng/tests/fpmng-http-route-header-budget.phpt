--TEST--
fpm-ng: the gateway does not forward a header block the target would refuse after the gateway's own additions (issue #466)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "fpmng-raw-upstream.inc";

const HEADERS_MAX = 64 * 1024;	/* FPM_HTTP_HEADERS_MAX */

function check(bool $condition, string $message, FpmngRaw $raw, string $extra = ''): void
{
    if (!$condition) {
        echo "FAIL: $message\n$extra\n--- log ---\n" . $raw->log() . "\n";
        exit(1);
    }
}

/* A request whose block costs libevent exactly $bytes: the request line and
 * each header line counted without CRLF, as the gateway's own inbound bound
 * and the target's both count them. */
function block(int $bytes): string
{
    $lines = ['GET /evil/x HTTP/1.1', 'Host: t'];
    $remaining = $bytes - array_sum(array_map('strlen', $lines));
    $count = (int) ceil($remaining / 900);
    for ($i = 0; $i < $count; $i++) {
        $len = intdiv($remaining, $count - $i);
        $name = sprintf('X-Pad-%04d: ', $i);
        $lines[] = $name . str_repeat('v', $len - strlen($name));
        $remaining -= $len;
    }

    return implode("\r\n", $lines) . "\r\n\r\n";
}

$raw = new FpmngRaw('hdrbudget');

/* ---- well inside the bound: forwarded ---------------------------------------- */
$raw->send(block(HEADERS_MAX - 1000));
$u = $raw->conn();
check($u !== null, 'a block well under the bound was not forwarded', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 200'), 'under the bound: not a 200', $raw, $resp);
echo "under-bound: forwarded\n";

/* ---- accepted inbound, but the forwarded block (X-Forwarded-*, Content-Length,
 * Connection added) would exceed the target's identical bound ------------------ */
$raw->send(block(HEADERS_MAX - 20), true);
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 400'), 'edge block: not a 400 from the gateway', $raw, $resp);
/* A forwarded request would travel on the kept-alive upstream connection (or
 * a new dial): neither may carry a byte. EOF on the old one is not a forward. */
$r = [$u, $raw->serverStream()];
$w = $e = null;
$ready = stream_select($r, $w, $e, 1);
$data = in_array($u, $r, true) ? (string) fread($u, 200) : '';
check($data === '' && !in_array($raw->serverStream(), $r, true),
    'edge block: the gateway forwarded a block the target would refuse', $raw, substr($data, 0, 60));
echo "edge-block: refused at the gateway, not forwarded\n";

$raw->finish();
?>
--EXPECT--
under-bound: forwarded
edge-block: refused at the gateway, not forwarded
--CLEAN--
<?php
require_once "fpmng-raw-upstream.inc";
FpmngRaw::clean('hdrbudget');
?>
