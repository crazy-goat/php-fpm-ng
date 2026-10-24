--TEST--
fpm-ng: listen.allowed_clients on an IPv6 listener compares all 128 bits; a client that matches an allowed address only in the first 96 bits is refused (GHSA-62xp-839h-2637)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
FPM\Tester::skipIfIPv6IsNotSupported();
?>
--FILE--
<?php

require_once "tester.inc";

/* php-src 8.5.9 compared only the first 12 of the 16 bytes of an IPv6 address
 * in fcgi_is_allowed() (main/fastcgi.c). The allowed address ::dead:beef has
 * twelve zero bytes, and so has the client ::1, so ::1 passed the check. php-src
 * 8.5.12 compares the whole address (sizeof(sin6_addr)). This build vendors
 * main/fastcgi.c from php-8.5.12 (third_party/php-src/MANIFEST).
 *
 * [exact] is the positive control: the client ::1 is allowed by ::1 and must
 * be served. [prefix] allows ::dead:beef. The client ::1 shares the first
 * 96 bits with it and must be refused. */
$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
[exact]
listen = {{ADDR:IPv6:ANY[exact]}}
listen.allowed_clients = ::1
pm = static
pm.max_children = 1
[prefix]
listen = {{ADDR:IPv6:ANY[prefix]}}
listen.allowed_clients = ::dead:beef
pm = static
pm.max_children = 1
catch_workers_output = yes
EOT;

$tester = new FPM\Tester($cfg);
$tester->start();
$tester->expectLogStartNotices();
$tester->checkRequest('{{ADDR:IPv6[exact]}}', 'exact: ok', 'exact: error');
$tester->checkRequest('{{ADDR:IPv6[prefix]}}', 'prefix: ok', 'prefix: error');
$tester->terminate();
$tester->expectLogWarning(
    'child %d said into stderr: "ERROR: Connection disallowed: IP address \'::1\' has been dropped."',
    'prefix'
);
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
exact: ok
prefix: error
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
