--TEST--
fpm-ng: the HTTP client transport names an oversize head as a protocol error and logs a half head followed by EOF (issue #463)
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

$raw = new FpmngRaw('headdiag');

/* ---- (A) a head above FPM_HTTP_HTTP_MAX_HEAD: the second line used to print
 * a stale errno (ENOENT) ------------------------------------------------------ */
$raw->send(fpmng_raw_get('/evil/a'));
$u = $raw->conn();
check($u !== null, 'the gateway never dialled the raw upstream', $raw);
$raw->readRequest($u);
@fwrite($u, "HTTP/1.1 200 OK\r\n");
$line = "X-Pad-" . str_repeat('a', 200) . ": " . str_repeat('b', 200) . "\r\n";
for ($sent = 0; $sent < 300 * 1024; ) {
    $n = @fwrite($u, $line);
    if (!$n) {
        break;
    }
    $sent += $n;
}
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 502'), 'oversize head: not a 502', $raw, $resp);
check($raw->waitLog("/sent a response head above 262144 bytes/") !== null, 'oversize head: no size line', $raw);
check($raw->waitLog("/http: upstream '[^']*headdiag\.sock': Protocol error/") !== null,
    'oversize head: the follow-up line does not say "Protocol error"', $raw);
echo "oversize-head: protocol error\n";

/* ---- (B) half a head, then EOF: a 502 with no log line at all ------------- */
$from = strlen($raw->log());
$raw->send(fpmng_raw_get('/evil/b'), true);
$u = $raw->conn();
check($u !== null, 'no second upstream connection', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nContent-Ty");
usleep(200000);
fclose($u);
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 502'), 'half head: not a 502', $raw, $resp);
check($raw->waitLog("/http: upstream '[^']*headdiag\.sock' closed in the middle of the response head/", $from) !== null,
    'half head: no log line', $raw);
echo "half-head: logged\n";

$raw->finish();
?>
--EXPECT--
oversize-head: protocol error
half-head: logged
--CLEAN--
<?php
require_once "fpmng-raw-upstream.inc";
FpmngRaw::clean('headdiag');
?>
