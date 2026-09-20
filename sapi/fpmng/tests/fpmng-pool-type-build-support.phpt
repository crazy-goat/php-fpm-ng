--TEST--
fpm-ng: every configurable pool type is accepted by a build that carries patches/0006 (issue #214, #388)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #214 introduced fpm_pool_type_check_build_support(): a type whose
 * children call zend_signal_use_persistent_handlers() (patches/0006, inside
 * Zend/) is refused by a binary linked against a distribution libphp, which
 * has no patch of ours in it.
 *
 * Before issue #388 two types set that capability bit: pool.type = http (the
 * proxy welded to a pool of PHP workers) and, before #376, fastcgi-ng. #388
 * retired http and split its proxy half into pool.type = gateway, which runs
 * no PHP child at all and therefore sets no capability bit. Nothing left in
 * this tree asks for patches/0006, so on BOTH builds -- the from-source one
 * and the libphp one -- every configurable type is accepted. That is what this
 * file now asserts. The guard itself stays in fpm_pool_type.c for the day a
 * type needs the patch again; it has no test because it has no trigger. */

function validate(string $extraConfig): ?string
{
    $cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
[pool]
listen = {{ADDR}}
pm = static
pm.max_children = 1
$extraConfig
EOT;

    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);

    return $messages === null ? null : implode("\n", $messages);
}

/* http-direct's validate() wants a document root holding a real front
 * controller. */
$root = sys_get_temp_dir() . '/fpmng-build-support-' . getmypid();
@mkdir($root);
file_put_contents($root . '/index.php', '<?php');

/* Issue #388: the gateway needs a target to route to, so it is its own
 * whole configuration rather than a suffix on the base pool above. */
$gatewayCfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
[gateway]
pool.type = gateway
listen = {{ADDR[http]}}
http.route[app] = /
[app]
listen = {{ADDR}}
pm = static
pm.max_children = 1
EOT;

foreach ([
    'fastcgi'     => 'pool.type = fastcgi',
    'http-direct' => "pool.type = http-direct\nchdir = $root\nhttp.front_controller = /index.php",
] as $label => $extraConfig) {
    $error = validate($extraConfig);
    if ($error !== null) {
        echo "FAIL: $label was refused by a build that must support it:\n$error\n";
        exit(1);
    }
    echo "$label: accepted\n";
}

$tester = new FPM\Tester($gatewayCfg, '<?php echo "ok";');
$messages = $tester->testConfig(true);
if ($messages !== null) {
    echo "FAIL: gateway was refused by a build that must support it:\n" . implode("\n", $messages) . "\n";
    exit(1);
}
echo "gateway: accepted\n";

@unlink($root . '/index.php');
@rmdir($root);

?>
Done
--EXPECT--
fastcgi: accepted
http-direct: accepted
gateway: accepted
Done
