--TEST--
fpm-ng: pool.type = gateway runs http.gateways proxy processes and no PHP child of its own (issue #388)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #388: the gateway is a type of its own, not a mode of pool.type = http.
 * Two things follow and are asserted here:
 *   - it has NO PHP child: the process manager never runs for it, even though
 *     its scoreboard allocator needs a non-zero pm.max_children (see
 *     fpm_http_validate_pool()); only the proxy processes exist;
 *   - its process count is http.gateways ALONE. The old http type clamped it
 *     to the pool's own pm.max_children (MIN(nproc_wanted, workers)); with the
 *     target below at pm.max_children = 1, that clamp would yield one gateway
 *     process, so "two" here is the regression guard. */

$docroot = sys_get_temp_dir() . '/fpmng-gateway-type-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo "gateway-ok";');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 2
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

$http = $tester->getAddr('ipv4', '[http]');
$body = @file_get_contents("http://$http/index.php");
if ($body !== 'gateway-ok') {
    echo "FAIL: gateway body=" . var_export($body, true) . "\n";
    exit(1);
}
echo "serves: gateway-ok\n";

/* The process table is the only place "no PHP child for gw" can be observed.
 * ps args shows the setproctitle line fpm_http_gateway_run() installs
 * ("http gateway <pool> [<i>]"), and $pool in an upstream worker's title. */
$ps = (string) shell_exec('ps -eo args 2>/dev/null');
$gateways = 0;
$ownWorkers = 0;
foreach (explode("\n", $ps) as $line) {
    if (str_contains($line, 'http gateway gw')) {
        $gateways++;
    }
    if (str_contains($line, 'pool gw')) {
        $ownWorkers++;
    }
}
if ($gateways !== 2) {
    echo "FAIL: expected exactly 2 gateway processes for gw, found $gateways\n$ps\n";
    exit(1);
}
echo "gateways: exactly 2\n";

if ($ownWorkers !== 0) {
    echo "FAIL: gw has a PHP child (the process manager must not run for a gateway)\n$ps\n";
    exit(1);
}
echo "own-php-children: none\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

/* Issue #388 finding 1: http.reuseport = on must work for a gateway. Every
 * member of a SO_REUSEPORT group has to set the option BEFORE bind(), so the
 * gateway can no longer inherit the master's socket (fpm_sockets_new_
 * listening_socket() sets only SO_REUSEADDR and is already listening): it
 * binds its own through fpm_http_listen(), which sets SO_REUSEPORT before
 * bind. This would fail every child's bind with EADDRINUSE before the fix. */
$cfgReuse = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 2
http.reuseport = on
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = $docroot
EOT;

$testerR = new FPM\Tester($cfgReuse, '<?php echo "unused";');
$testerR->start();
$testerR->expectLogStartNotices();
$httpR = $testerR->getAddr('ipv4', '[http]');
$bodyR = @file_get_contents("http://$httpR/index.php");
if ($bodyR !== 'gateway-ok') {
    echo "FAIL: reuseport gateway body=" . var_export($bodyR, true) . "\n";
    exit(1);
}
echo "reuseport-serves: gateway-ok\n";
$testerR->terminate();
$testerR->expectLogTerminatingNotices();
$testerR->close();

@unlink($docroot . '/index.php');
@rmdir($docroot);

?>
Done
--EXPECT--
serves: gateway-ok
gateways: exactly 2
own-php-children: none
reuseport-serves: gateway-ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
