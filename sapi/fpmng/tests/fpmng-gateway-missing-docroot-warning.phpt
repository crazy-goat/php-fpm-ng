--TEST--
fpm-ng: gateway with a FastCGI target but no chdir and no http.front_controller warns at startup (issue #736)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #736: a gateway pool that routes to a fastcgi pool builds
 * SCRIPT_FILENAME as docroot + decoded path, and the docroot is the pool's
 * chdir -- or the master's working directory when chdir is unset. With
 * neither chdir nor http.front_controller set, every request names a script
 * that does not exist and the upstream answers "Primary script unknown",
 * with nothing pointing at the gateway's configuration. The master says so
 * once at startup, before any request is ever sent. */

$warning = "/WARNING: \\[pool gw\\] http: no chdir and no http\\.front_controller with a FastCGI target/";

$docroot = sys_get_temp_dir() . '/fpmng-gateway-docroot-' . getmypid();
@mkdir($docroot, 0700, true);
file_put_contents($docroot . '/index.php', '<?php echo "unused";');

$cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
[gw]
pool.type = gateway
listen = {{ADDR[gw]}}
http.gateways = 1
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = $docroot
EOT;

$tester = new FPM\Tester($cfg, '<?php');
$tester->start();
$tester->expectLogStartNotices();
// checkAllLogs: the warning is logged before the "ready to handle
// connections" notice expectLogStartNotices() just consumed -- rescan from
// the beginning instead of only reading forward from here.
$tester->expectLogPattern($warning, true);
echo "missing-docroot: warning seen before any request was ever sent\n";

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

// Negative: the same gateway with a chdir stays silent -- a warning that
// fires on a working configuration is worse than none.
$cfg2 = <<<EOT
[global]
error_log = {{FILE:LOG}}
[gw]
pool.type = gateway
listen = {{ADDR[gw2]}}
chdir = $docroot
http.gateways = 1
http.route[app] = /
[app]
pool.type = fastcgi
listen = {{ADDR}}
pm = static
pm.max_children = 1
chdir = $docroot
EOT;

$tester2 = new FPM\Tester($cfg2, '<?php');
$tester2->start();
$tester2->expectLogStartNotices();
$tester2->expectNoLogPattern($warning, true);
echo "with-chdir: no warning\n";

$tester2->terminate();
$tester2->expectLogTerminatingNotices();
$tester2->close();

@unlink($docroot . '/index.php');
@rmdir($docroot);

?>
Done
--EXPECT--
missing-docroot: warning seen before any request was ever sent
with-chdir: no warning
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
