--TEST--
fpm-ng: a route target whose socket is gone answers 502 and names the connect error, not "pool full" 503 (issue #465)
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

$fcSock = sys_get_temp_dir() . '/fpmng-raw-dead-fc.sock';
$docRoot = FpmngRaw::docRoot('dead');
$raw = new FpmngRaw('dead', <<<EOT
[fc]
pool.type = fastcgi
listen = $fcSock
chdir = $docRoot
pm = static
pm.max_children = 1
EOT, "http.route[fc] = /fc\n");

/* ---- http-direct target, socket removed after start ------------------------ */
$raw->killTarget();
$from = strlen($raw->log());
$raw->send(fpmng_raw_get('/evil/a'));
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 502'), 'http target: not a 502', $raw, $resp);
check(!preg_match('/^Retry-After:/mi', FpmngRaw::head($resp)), 'http target: Retry-After on a 502', $raw, $resp);
check($raw->waitLog("/http: cannot connect to target '[^']*dead\.sock' \(No such file or directory\)/", $from) !== null,
    'http target: the connect error is not logged', $raw);
check(!str_contains(substr($raw->log(), $from), 'pool full'), 'http target: reported as pool full', $raw);
echo "http-target: 502, connect error logged\n";

/* ---- FastCGI target, socket removed after start --------------------------- */
@unlink($fcSock);
$from = strlen($raw->log());
$raw->send(fpmng_raw_get('/fc/x'), true);
$resp = $raw->readResponse();
check(str_starts_with($resp, 'HTTP/1.1 502'), 'fastcgi target: not a 502', $raw, $resp);
check($raw->waitLog("/http: cannot connect to target '[^']*dead-fc\.sock' \(No such file or directory\)/", $from) !== null,
    'fastcgi target: the connect error is not logged', $raw);
check(!str_contains(substr($raw->log(), $from), 'pool full'), 'fastcgi target: reported as pool full', $raw);
echo "fastcgi-target: 502, connect error logged\n";

$raw->finish();
?>
--EXPECT--
http-target: 502, connect error logged
fastcgi-target: 502, connect error logged
--CLEAN--
<?php
require_once "fpmng-raw-upstream.inc";
FpmngRaw::clean('dead');
@unlink(sys_get_temp_dir() . '/fpmng-raw-dead-fc.sock');
?>
