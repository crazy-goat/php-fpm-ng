--TEST--
fpm-ng: a gateway error reply (404 with no route, 504 from a silent target, 502 from a gone target) carries the request's X-Request-Id and the access log names it (issue #882)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "fpmng-raw-upstream.inc";

/* Issue #882: the gateway wrote its own error replies with evhttp_send_error(),
 * which clears the output headers and with them the X-Request-Id that
 * fpm_http_client_request_begin() set. Each reply below is one kind of error the
 * gateway writes itself, and each must carry the id the access log line names:
 *
 *   [404]    no http.route[] entry claims the path: answered locally;
 *   [502 a]  the target reads the request and closes without any answer;
 *   [502 b]  the target sends an unfinished head and closes (issue #636's
 *            shape, which the http transport answers as no answer);
 *   [504]    the target took the request and never answered, and
 *            http.upstream_read_timeout cut it before any head was sent;
 *   [502 c]  the target's socket is gone: the connect fails.
 *
 * The raw target is the same for all of them. The 504 case leaves a connection
 * the gateway dialled and the test did not accept, so it runs after the cases
 * that accept their own connection. The access log is on, so the id is checked
 * on both sides. */

function check(bool $ok, string $what, FpmngRaw $raw, string $extra = ''): void
{
    if (!$ok) {
        echo "FAIL: $what\n$extra\n--- error log ---\n" . $raw->log() . "\n";
        exit(1);
    }
}

/* The id from the X-Request-Id response header, or null. */
function requestId(string $resp): ?string
{
    return preg_match('/^X-Request-Id:[ \t]*(\S+)/mi', FpmngRaw::head($resp), $m) ? $m[1] : null;
}

/* Waits for the access log line of this request; '' when it never appears. */
function accessLine(string $file, string $id): string
{
    $deadline = microtime(true) + 10;
    do {
        foreach (explode("\n", (string) @file_get_contents($file)) as $line) {
            if (str_contains($line, "request_id=$id")) {
                return $line;
            }
        }
        usleep(100000);
    } while (microtime(true) < $deadline);

    return '';
}

/* The bytes field of an access log line: the number after the status. */
function accessBytes(string $line): ?int
{
    return preg_match('/" \d{3} (\d+) /', $line, $m) ? (int) $m[1] : null;
}

$raw = new FpmngRaw('error-id', '', <<<EOT
http.request_id = generate
http.upstream_read_timeout = 1500
http.access_log = {{FILE:LOG:ACC}}
EOT);
$accessLog = $raw->tester->getPrefixedFile(FPM\Tester::FILE_EXT_LOG_ACC);

/* 404: nothing claims /nothing, so the gateway answers without a target. */
$raw->send(fpmng_raw_get('/nothing'));
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 404'), '404: not a 404', $raw, $resp);
$id = requestId($resp);
check($id !== null && preg_match('/^[0-9a-f]{32}$/', $id) === 1, '404: no generated X-Request-Id', $raw, $resp);
$line = accessLine($accessLog, $id);
check(str_contains($line, "request_id=$id"), '404: access log does not name the id', $raw);
/* "Not Found\n" is 10 bytes. Before issue #882 the log said 0. */
check(FpmngRaw::body($resp) === "Not Found\n", '404: unexpected body', $raw, $resp);
check(accessBytes($line) === 10, '404: access log bytes is ' . var_export(accessBytes($line), true) . ', want 10', $raw, $line);
echo "404: X-Request-Id kept, access log names it, bytes counted\n";

/* HEAD to the same path: no body on the wire, and 0 in the log. libevent wrote
 * the body of a HEAD reply before issue #882. */
$raw->send("HEAD /nothing HTTP/1.1\r\nHost: t\r\n\r\n", true);
[$resp] = $raw->readUntilClose();
check(str_starts_with($resp, 'HTTP/1.1 404'), 'HEAD 404: not a 404', $raw, $resp);
check(FpmngRaw::body($resp) === '', 'HEAD 404: a body went on the wire', $raw, $resp);
$id = requestId($resp);
check($id !== null, 'HEAD 404: no X-Request-Id', $raw, $resp);
$line = accessLine($accessLog, $id);
check(accessBytes($line) === 0, 'HEAD 404: access log bytes is ' . var_export(accessBytes($line), true) . ', want 0', $raw, $line);
echo "HEAD 404: no body, bytes 0\n";

/* 502, no upstream answer: the target reads the request and closes. */
$raw->send(fpmng_raw_get('/evil/noanswer'), true);
$u = $raw->conn();
check($u !== null, 'no-answer: the gateway never dialled the raw target', $raw);
$raw->readRequest($u);
fclose($u);
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 502'), 'no-answer: not a 502', $raw, $resp);
$id = requestId($resp);
check($id !== null && preg_match('/^[0-9a-f]{32}$/', $id) === 1, 'no-answer: no generated X-Request-Id', $raw, $resp);
$line = accessLine($accessLog, $id);
check(str_contains($line, "request_id=$id"), 'no-answer: access log does not name the id', $raw);
check(accessBytes($line) === 12, 'no-answer: access log bytes is ' . var_export(accessBytes($line), true) . ', want 12', $raw, $line);
echo "502 no answer: X-Request-Id kept, access log names it\n";

/* 502, unfinished head: the target sends part of a head and closes. */
$raw->send(fpmng_raw_get('/evil/halfhead'), true);
$u = $raw->conn();
check($u !== null, 'partial-head: the gateway never dialled the raw target', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nX-Partial: yes");
usleep(150000);
fclose($u);
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 502'), 'partial-head: not a 502', $raw, $resp);
$id = requestId($resp);
check($id !== null && preg_match('/^[0-9a-f]{32}$/', $id) === 1, 'partial-head: no generated X-Request-Id', $raw, $resp);
$line = accessLine($accessLog, $id);
check(str_contains($line, "request_id=$id"), 'partial-head: access log does not name the id', $raw);
check(accessBytes($line) === 12, 'partial-head: access log bytes is ' . var_export(accessBytes($line), true) . ', want 12', $raw, $line);
echo "502 partial head: X-Request-Id kept, access log names it\n";

/* 504: the target reads nothing back; the gateway cuts it after 1500 ms. */
$raw->send(fpmng_raw_get('/evil/silent'), true);
$resp = $raw->readResponse(10.0);
check(str_starts_with($resp, 'HTTP/1.1 504'), '504: not a 504', $raw, $resp);
$id = requestId($resp);
check($id !== null && preg_match('/^[0-9a-f]{32}$/', $id) === 1, '504: no generated X-Request-Id', $raw, $resp);
$line = accessLine($accessLog, $id);
check(str_contains($line, "request_id=$id"), '504: access log does not name the id', $raw);
check(accessBytes($line) === 16, '504: access log bytes is ' . var_export(accessBytes($line), true) . ', want 16', $raw, $line);
echo "504: X-Request-Id kept, access log names it\n";

/* 502: the target's socket is removed, so the connect fails. */
$raw->killTarget();
$raw->send(fpmng_raw_get('/evil/gone'), true);
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 502'), '502: not a 502', $raw, $resp);
$id = requestId($resp);
check($id !== null && preg_match('/^[0-9a-f]{32}$/', $id) === 1, '502: no generated X-Request-Id', $raw, $resp);
$line = accessLine($accessLog, $id);
check(str_contains($line, "request_id=$id"), '502: access log does not name the id', $raw);
check(accessBytes($line) === 12, '502: access log bytes is ' . var_export(accessBytes($line), true) . ', want 12', $raw, $line);
echo "502: X-Request-Id kept, access log names it\n";

$raw->finish();
?>
--EXPECT--
404: X-Request-Id kept, access log names it, bytes counted
HEAD 404: no body, bytes 0
502 no answer: X-Request-Id kept, access log names it
502 partial head: X-Request-Id kept, access log names it
504: X-Request-Id kept, access log names it
502: X-Request-Id kept, access log names it
--CLEAN--
<?php
require_once "fpmng-raw-upstream.inc";
FpmngRaw::clean('error-id');
?>
