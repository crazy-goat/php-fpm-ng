--TEST--
fpm-ng: legal pool.type values with the classic executor start, serve one request, and shut down (docs/NOTES.md §3i)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

$dir = __DIR__;

/* Issue #388: the classic-executor matrix used to exercise pool.type = http,
 * the gateway welded to its own pool of PHP workers. That type is retired; the
 * proxy half is pool.type = gateway and the worker half is an ordinary
 * fastcgi pool, so the shape exercised here is a gateway routing to one
 * fastcgi target over its public port. */
$label = 'gateway-classic';
$docroot = sys_get_temp_dir() . '/fpmng-classic-matrix-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo "' . $label . '";');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = $docroot
EOT;

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
$tester->start();
$tester->expectLogStartNotices();

$addr = $tester->getAddr('ipv4', '[http]');
$body = @file_get_contents("http://$addr/index.php");
if ($body !== $label) {
    echo "FAIL: $label body=" . var_export($body, true) . "\n";
    exit(1);
}

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

echo "$label: ok\n";

@unlink($docroot . '/index.php');
@rmdir($docroot);

?>
Done
--EXPECT--
gateway-classic: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
