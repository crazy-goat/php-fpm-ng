--TEST--
fpm-ng: an absolute-form request-target reaches both route transports as origin-form (issue #462, RFC 9112 3.2.2)
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

/* /evil is the raw upstream (what the gateway puts on the wire, byte-exact);
 * /direct is a real http-direct pool and /fc a fastcgi one, both echoing the
 * REQUEST_URI their application sees. */
$docRoot = FpmngRaw::docRoot('absform');
$raw = new FpmngRaw('absform', <<<EOT
[direct]
pool.type = http-direct
listen = {{ADDR[direct]}}
chdir = $docRoot
pm = static
pm.max_children = 1
http.front_controller = /index.php
[fc]
pool.type = fastcgi
listen = {{ADDR[fc]}}
chdir = $docRoot
pm = static
pm.max_children = 1
EOT, "http.route[direct] = /direct\nhttp.route[fc] = /\nhttp.front_controller = /index.php");

/* the wire */
$raw->send("GET http://t/evil/x?a=1 HTTP/1.1\r\nHost: t\r\n\r\n");
$u = $raw->accept();
check($u !== null, 'the gateway never dialled the raw upstream', $raw);
$req = $raw->readRequest($u);
check(str_starts_with($req, "GET /evil/x?a=1 HTTP/1.1\r\n"), 'the raw upstream was sent a non-origin-form target', $raw, $req);
fwrite($u, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
check(str_starts_with($raw->readResponse(), 'HTTP/1.1 200'), 'wire: the client got no 200', $raw);
echo "wire: " . strtok($req, "\r") . "\n";

/* the same client request through a real http-direct target and a fastcgi one */
foreach (['direct', 'fc'] as $route) {
    $raw->send("GET http://t/$route/x?a=1 HTTP/1.1\r\nHost: t\r\n\r\n", true);
    $resp = $raw->readResponse();
    check(str_starts_with($resp, 'HTTP/1.1 200'), "$route: " . FpmngRaw::status($resp), $raw, $resp);
    $body = FpmngRaw::body($resp);
    echo "$route: $body\n";
}

$raw->finish();
?>
--EXPECT--
wire: GET /evil/x?a=1 HTTP/1.1
direct: /direct/x?a=1
fc: /fc/x?a=1
--CLEAN--
<?php
require_once "fpmng-raw-upstream.inc";
FpmngRaw::clean('absform');
?>
