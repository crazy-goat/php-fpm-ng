--TEST--
fpm-ng: http.response_header[] and http.error_page[] on a route to an http-direct target (the HTTP transport), including an upstream header of the same name (issue #650)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
fpmng_skip_if_pool_type_unsupported('http-direct');
?>
--FILE--
<?php

require_once "fpmng-raw-upstream.inc";

/* Issue #650. The FastCGI transport and the HTTP transport share the loop that
 * copies the upstream headers (fpm_http_start_reply()), so the drop of an
 * upstream header with a configured name has to hold on both. The scripted
 * target sends its own Strict-Transport-Security twice. */

function check(bool $ok, string $what, FpmngRaw $raw, string $extra = ''): void
{
    if (!$ok) {
        echo "FAIL: $what\n$extra\n--- error log ---\n" . $raw->log() . "\n";
        exit(1);
    }
}

/* The values of one response header, in order. */
function values(string $resp, string $name): array
{
    preg_match_all('/^' . preg_quote($name, '/') . ':[ \t]*(.*?)\r?$/mi', FpmngRaw::head($resp), $m);
    return $m[1];
}

$dir = sys_get_temp_dir() . '/fpmng-raw-header-pages-' . getmypid();
@mkdir($dir, 0700, true);
file_put_contents("$dir/502.html", '<h1>upstream failed</h1>');

$raw = new FpmngRaw('resp-header', '', <<<EOT
http.response_header[Strict-Transport-Security] = "max-age=31536000; includeSubDomains"
http.response_header[X-Frame-Options] = DENY
http.error_page[502] = $dir/502.html
EOT);

$raw->send(fpmng_raw_get('/evil/ok'));
$u = $raw->conn();
check($u !== null, 'the gateway never dialled the raw target', $raw);
$raw->readRequest($u);
fwrite($u, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nStrict-Transport-Security: max-age=1\r\n"
    . "Strict-Transport-Security: max-age=2\r\nX-Frame-Options: SAMEORIGIN\r\nX-Other: kept\r\n\r\nfine");
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 200'), '200: not a 200', $raw, $resp);
check(values($resp, 'Strict-Transport-Security') === ['max-age=31536000; includeSubDomains'], '200: HSTS is not the configured value, once', $raw, $resp);
check(values($resp, 'X-Frame-Options') === ['DENY'], '200: X-Frame-Options is not the configured value, once', $raw, $resp);
check(values($resp, 'X-Other') === ['kept'], '200: an upstream header with another name was lost', $raw, $resp);
check(FpmngRaw::body($resp) === 'fine', '200: body', $raw, $resp);
fclose($u);
echo "200 over the HTTP transport: configured values only, other headers kept\n";

/* 502: the target reads the request and closes. */
$raw->send(fpmng_raw_get('/evil/noanswer'), true);
$u = $raw->conn();
check($u !== null, 'no-answer: the gateway never dialled the raw target', $raw);
$raw->readRequest($u);
fclose($u);
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 502'), 'no-answer: not a 502', $raw, $resp);
check(values($resp, 'Strict-Transport-Security') === ['max-age=31536000; includeSubDomains'], '502: HSTS', $raw, $resp);
check(FpmngRaw::body($resp) === '<h1>upstream failed</h1>', '502: body is not the page', $raw, $resp);
check(values($resp, 'Content-Type') === ['text/html; charset=UTF-8'], '502: Content-Type', $raw, $resp);
echo "502 no answer: page and headers once\n";

/* 502: the socket is gone. */
$raw->killTarget();
$raw->send(fpmng_raw_get('/evil/gone'), true);
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 502'), 'gone: not a 502', $raw, $resp);
check(values($resp, 'X-Frame-Options') === ['DENY'], 'gone: X-Frame-Options', $raw, $resp);
check(FpmngRaw::body($resp) === '<h1>upstream failed</h1>', 'gone: body is not the page', $raw, $resp);
echo "502 gone: page and headers once\n";

@unlink("$dir/502.html");
@rmdir($dir);
$raw->finish();
?>
--EXPECT--
200 over the HTTP transport: configured values only, other headers kept
502 no answer: page and headers once
502 gone: page and headers once
--CLEAN--
<?php
require_once "fpmng-raw-upstream.inc";
FpmngRaw::clean('resp-header');
?>
