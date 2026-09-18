--TEST--
fpm-ng: the pool types a build cannot honour are refused, the rest are accepted (issue #214)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php

require_once "tester.inc";

/* Two builds produce this binary and they support different sets of pool
 * types. A build from patched source accepts all three. The libphp build
 * (build/libphp-build.sh) links against a distribution's libphp, which carries
 * no patch of ours inside Zend/, so zend_signal_use_persistent_handlers()
 * (patches/0006) is not in it and every type with reuses_request_runtime set
 * refuses to start rather than run on upstream signal behaviour under a name
 * that promises the opposite.
 *
 * Which build this is, is asked of the binary rather than derived from its
 * configure line: on the libphp path `Configure Command` is the DISTRIBUTION's
 * (it is libphp's own, ours never ran configure), which is exactly what makes
 * issue #215 a bug. The refusal message is the binary's own answer to the
 * question, and checking it is also what keeps a refusal for some OTHER reason
 * from being read as this one. */
const REASON = 'does not carry patches/0006';

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
 * controller. It runs after the check under test, so without one this test
 * would fail on a missing file and say nothing about pool types. */
$root = sys_get_temp_dir() . '/fpmng-build-support-' . getmypid();
@mkdir($root);
file_put_contents($root . '/index.php', '<?php');

$configs = [
    'fastcgi'     => 'pool.type = fastcgi',
    'http-direct' => "pool.type = http-direct\nchdir = $root\nhttp.front_controller = /index.php",
    'http'        => "pool.type = http\nhttp.listen = {{ADDR[http]}}",
];

/* The libphp guard is keyed off the capability bit (reuses_request_runtime),
 * so http is the one type left that decides which build this is; the other
 * two are then held to the opposite answer. Issue #376 removed fastcgi-ng,
 * which used to stand beside http here. */
$probe = validate($configs['http']);
$libphpBuild = $probe !== null && str_contains($probe, REASON);

foreach ($configs as $label => $extraConfig) {
    $needsPatch = $label === 'http';
    $error = $label === 'http' ? $probe : validate($extraConfig);

    if ($libphpBuild && $needsPatch) {
        if ($error === null) {
            echo "FAIL: $label was accepted by a build that refused the other type needing patches/0006\n";
            exit(1);
        }
        if (!str_contains($error, REASON)) {
            echo "FAIL: $label was refused, but not for the reason this build has:\n$error\n";
            exit(1);
        }
        echo "$label: refused, patches/0006 is not in this libphp\n";
        continue;
    }

    if ($error !== null) {
        echo "FAIL: $label was refused by a build that must support it\n";
        if (str_contains($error, REASON)) {
            echo "and the reason is the libphp guard, which must never reach a source build:\n";
        }
        echo "$error\n";
        exit(1);
    }
    echo "$label: accepted\n";
}

@unlink($root . '/index.php');
@rmdir($root);

/* The EXPECTF section below is exact for fastcgi and http-direct -- those are
 * what both builds exist to serve, and a refusal of either is a bug anywhere.
 * The other line is %s because the right answer differs by build and a
 * .phpt cannot know which one is running it. Nothing is lost: every wrong
 * answer above exits non-zero with the reason before the comparison happens,
 * so %s only ever absorbs the two spellings of a correct one. */

?>
Done
--EXPECTF--
fastcgi: accepted
http-direct: accepted
http: %s
Done
