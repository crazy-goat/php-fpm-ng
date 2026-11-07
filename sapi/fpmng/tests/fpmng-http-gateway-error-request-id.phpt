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
 *   [404]  no http.route[] entry claims the path: answered locally;
 *   [504]  the target took the request and never answered, and
 *          http.upstream_read_timeout cut it before any head was sent;
 *   [502]  the target's socket is gone: the connect fails.
 *
 * The 504 and the 502 use the same raw target: it is silent until the test
 * removes its socket. The access log is on, so the id is checked on both sides. */

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
check(str_contains(accessLine($accessLog, $id), "request_id=$id"), '404: access log does not name the id', $raw);
echo "404: X-Request-Id kept, access log names it\n";

/* 504: the target reads nothing back; the gateway cuts it after 1500 ms. */
$raw->send(fpmng_raw_get('/evil/silent'), true);
$resp = $raw->readResponse(10.0);
check(str_starts_with($resp, 'HTTP/1.1 504'), '504: not a 504', $raw, $resp);
$id = requestId($resp);
check($id !== null && preg_match('/^[0-9a-f]{32}$/', $id) === 1, '504: no generated X-Request-Id', $raw, $resp);
check(str_contains(accessLine($accessLog, $id), "request_id=$id"), '504: access log does not name the id', $raw);
echo "504: X-Request-Id kept, access log names it\n";

/* 502: the target's socket is removed, so the connect fails. */
$raw->killTarget();
$raw->send(fpmng_raw_get('/evil/gone'), true);
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 502'), '502: not a 502', $raw, $resp);
$id = requestId($resp);
check($id !== null && preg_match('/^[0-9a-f]{32}$/', $id) === 1, '502: no generated X-Request-Id', $raw, $resp);
check(str_contains(accessLine($accessLog, $id), "request_id=$id"), '502: access log does not name the id', $raw);
echo "502: X-Request-Id kept, access log names it\n";

$raw->finish();
?>
--EXPECT--
404: X-Request-Id kept, access log names it
504: X-Request-Id kept, access log names it
502: X-Request-Id kept, access log names it
--CLEAN--
<?php
require_once "fpmng-raw-upstream.inc";
FpmngRaw::clean('error-id');
?>
