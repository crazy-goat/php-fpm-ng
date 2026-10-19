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

/* A second php-fpm-ng on this host with a pool named gw, running while the
 * count below is taken (issue #749). Its two gateways are visible to a bare
 * args match, so the count is exact only if the ppid filter excludes them.
 * Each Tester numbers its ports from 9008 on its own, so the decoy first
 * takes two numbers: $tester owns the two above. The Tester gives a gateway
 * pool that names no operator.*_listen a default listener, and $tester holds
 * it, so the decoy names its own two. */
$decoyCfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[gw]
pool.type = gateway
listen = {{ADDR[http]}}
chdir = $docroot
http.gateways = 2
http.route[app] = /
operator.status_listen = {{ADDR[dstatus]}}
operator.metrics_listen = {{ADDR[dmetrics]}}
[app]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = $docroot
EOT;
/* The decoy takes its own file prefix: with the default one its pid and
 * error_log would be $tester's {{FILE:PID}} and {{FILE:LOG}}, and getPid()
 * would return the decoy master. clean() does not know this prefix, so the
 * files are removed at the end of the test. */
$decoy = new FPM\Tester($decoyCfg, '<?php echo "unused";', [], $docroot . '/decoy.');
$decoy->getAddr('ipv4', 'skip-a');
$decoy->getAddr('ipv4', 'skip-b');
$decoy->start();
$decoy->expectLogStartNotices();
$decoyHttp = $decoy->getAddr('ipv4', '[http]');
$decoyBody = @file_get_contents("http://$decoyHttp/index.php");
if ($decoyBody !== 'gateway-ok') {
    echo "FAIL: decoy gateway body=" . var_export($decoyBody, true) . "\n";
    exit(1);
}

/* The process table is the only place "no PHP child for gw" can be observed.
 * ps args shows the setproctitle line fpm_http_gateway_run() installs
 * ("http gateway <pool> [<i>]"), and $pool in an upstream worker's title.
 *
 * Only THIS master's children are counted, by ppid (its pid is in the pid
 * file). A bare args match finds every php-fpm-ng on the host that happens to
 * have a pool named gw, which is what the two gateway-counters tests already
 * avoid: on 2026-10-06, two owned suites in one lane pair on one host, this
 * one counted the other lane's two gateways and failed with "expected exactly
 * 2 gateway processes for gw, found 3". */
$ps = (string) shell_exec('ps -eo pid,ppid,args 2>/dev/null');
$masterPid = $tester->getPid();
$gateways = 0;
$ownWorkers = 0;
foreach (explode("\n", $ps) as $line) {
    if (!preg_match('/^\s*(\d+)\s+(\d+)\s+(.*)$/', $line, $m) || (int) $m[2] !== $masterPid) {
        continue;
    }
    if (str_contains($m[3], 'http gateway gw')) {
        $gateways++;
    }
    if (str_contains($m[3], 'pool gw')) {
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

/* Without this, a decoy that never came up would make the count above
 * pass for the wrong reason. Both masters' gateways must be in ps. */
$machineGateways = 0;
foreach (explode("\n", $ps) as $line) {
    if (str_contains($line, 'http gateway gw')) {
        $machineGateways++;
    }
}
if ($machineGateways < 4) {
    echo "FAIL: decoy gateways not visible in ps (machine-wide $machineGateways, expected at least 4)\n$ps\n";
    exit(1);
}

$decoy->terminate();
$decoy->expectLogTerminatingNotices();
$decoy->close();

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

foreach (glob($docroot . '/decoy.*') as $decoyFile) {
    @unlink($decoyFile);
}
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
