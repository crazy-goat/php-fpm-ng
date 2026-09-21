--TEST--
fpm-ng: every configurable pool type is accepted and every retired name migrates (issues #214, #388, #420)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #214 introduced fpm_pool_type_check_build_support(): a type whose
 * children call zend_signal_use_persistent_handlers() (patches/0006, inside
 * Zend/) was refused by a binary linked against a distribution libphp, which
 * has no patch of ours in it.
 *
 * Before issue #388 two types set that capability bit: pool.type = http (the
 * proxy welded to a pool of PHP workers) and, before #376, fastcgi-ng. #388
 * retired http and split its proxy half into pool.type = gateway, which runs
 * no PHP child at all and therefore sets no capability bit. Nothing left in
 * this tree asked for patches/0006, so issue #420 removed the patch, the
 * capability field and the guard with it: no type needs a build-time
 * capability any more.
 *
 * That is what this file pins. Two halves:
 *   - every configurable type (fastcgi, http-direct, gateway) is accepted by
 *     -t on this build;
 *   - the retired names keep their actionable migration errors, because a
 *     retired name is a config file that used to start, not a typo. #420 did
 *     not touch those (fpm_pool_type_retired(), fpm_pool_types_retired[]). */

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

/* A bare pool of the named type, for the retired-name cases: the refusal comes
 * from fpm_pool_type_get()/fpm_pool_type_retired() before any directive of the
 * pool is validated, so the base above is enough. */
function retired(string $type): ?string
{
    $cfg = <<<EOT
[global]
error_log = {{FILE:LOG}}
[pool]
listen = {{ADDR}}
pm = static
pm.max_children = 1
pool.type = $type
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

/* Issue #388/#420: the retired names keep their migration errors, the shape of
 * the replacement included. */
$retired = [
    'http'       => ["pool.type 'http' no longer exists", 'pool.type = gateway'],
    'fastcgi-ng' => ["pool.type 'fastcgi-ng' no longer exists", 'removed in 0.9.0 (issue #376)'],
    'status'     => ["pool.type 'status' no longer exists", 'operator.status_path'],
];
foreach ($retired as $label => $needles) {
    $text = retired($label);
    if ($text === null) {
        echo "FAIL: retired pool.type = $label was accepted\n";
        exit(1);
    }
    foreach ($needles as $needle) {
        if (!str_contains($text, $needle)) {
            echo "FAIL: retired pool.type = $label missing needle: $needle\ngot:\n$text\n";
            exit(1);
        }
    }
    echo "$label: retired with migration error\n";
}

@unlink($root . '/index.php');
@rmdir($root);

?>
Done
--EXPECT--
fastcgi: accepted
http-direct: accepted
gateway: accepted
http: retired with migration error
fastcgi-ng: retired with migration error
status: retired with migration error
Done
