--TEST--
fpm-ng: bytes after a completed upstream response are logged and end the connection, never re-parsed as a later response (issue #464)
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

$raw = new FpmngRaw('leftover');
$pattern = "/http: upstream '[^']*leftover\.sock' sent unsolicited bytes after a completed response/";

/* ---- (1) the garbage rides in the same read as head + body (the head+tail path) */
$from = strlen($raw->log());
$raw->send(fpmng_raw_get('/evil/a'));
$u = $raw->conn();
check($u !== null, 'the gateway never dialled the raw upstream', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloGARBAGE");
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 200') && str_ends_with($resp, 'hello'), 'head+tail: the response itself was not delivered', $raw, $resp);
check($raw->waitLog($pattern, $from) !== null, 'head+tail: no log line for the leftover bytes', $raw);
check($raw->closedByGateway($u), 'head+tail: the gateway kept the connection', $raw);
echo "head-and-tail: logged, connection ended\n";

/* ---- (2) the garbage rides in the same read as the end of the body ------- */
$from = strlen($raw->log());
$raw->send(fpmng_raw_get('/evil/b'), true);
$u = $raw->conn();
check($u !== null, 'no second upstream connection', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello");
usleep(200000);
fwrite($u, "worldGARBAGE");
$resp = $raw->readResponse();
check(str_ends_with($resp, 'helloworld'), 'body: the response itself was not delivered', $raw, $resp);
check($raw->waitLog($pattern, $from) !== null, 'body: no log line for the leftover bytes', $raw);
check($raw->closedByGateway($u), 'body: the gateway kept the connection', $raw);
echo "body: logged, connection ended\n";

/* ---- (3) the next request still gets its own answer on a fresh connection - */
$raw->send(fpmng_raw_get('/evil/c'), true);
$u = $raw->conn();
check($u !== null, 'no third upstream connection', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nresp-C");
$resp = $raw->readResponse();
check(str_ends_with($resp, 'resp-C'), 'request C did not get its own response', $raw, $resp);
echo "next-request: own response\n";

/* ---- (4) bytes on an idle connection after a clean response ---------------- */
$from = strlen($raw->log());
usleep(200000);
@fwrite($u, "LATE");
check($raw->waitLog("/sent response bytes with no request to attach them to/", $from) !== null, 'idle: no log line', $raw);
echo "idle: logged\n";

$raw->finish();
?>
--EXPECT--
head-and-tail: logged, connection ended
body: logged, connection ended
next-request: own response
idle: logged
--CLEAN--
<?php
require_once "fpmng-raw-upstream.inc";
FpmngRaw::clean('leftover');
?>
