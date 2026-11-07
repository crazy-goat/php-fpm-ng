--TEST--
fpm-ng: http.sendfile_root is refused on the types it does not apply to and checked on gateway (#649)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #649: http.sendfile_root applies to pool.type = gateway only. A
 * FastCGI worker takes the http.* family as refused by name, and an
 * http-direct pool reads none of the gateway's upstream headers, so it is
 * refused there too (FPM_HTTP_DIRECT_REJECTS_COMMON). */

function expectConfigFailure(string $label, string $cfg, array $needles): void
{
    $tester = new FPM\Tester($cfg, '<?php echo "ok";');
    $messages = $tester->testConfig(true);
    if ($messages === null) {
        echo "FAIL: $label unexpectedly passed validation\n";
        exit(1);
    }
    $text = implode("\n", $messages);
    foreach ($needles as $needle) {
        if (!str_contains($text, $needle)) {
            echo "FAIL: $label missing needle: $needle\n";
            echo "got:\n$text\n";
            exit(1);
        }
    }
    echo "$label: rejected\n";
}

$base = <<<EOT
[global]
error_log = {{FILE:LOG}}
[pool]
listen = {{ADDR}}
pm = static
pm.max_children = 1
EOT;

expectConfigFailure(
    'fastcgi-sendfile-root',
    $base . "\nhttp.sendfile_root = " . sys_get_temp_dir(),
    ["'http.sendfile_root' is not supported by pool.type = fastcgi"]
);

expectConfigFailure(
    'http-direct-sendfile-root',
    "[global]\nerror_log = {{FILE:LOG}}\n[pool]\npool.type = http-direct\nlisten = {{ADDR}}\nchdir = " .
        sys_get_temp_dir() . "\nhttp.sendfile_root = " . sys_get_temp_dir(),
    ["'http.sendfile_root' is not supported by pool.type = http-direct"]
);

expectConfigFailure(
    'gateway-relative-root',
    "[global]\nerror_log = {{FILE:LOG}}\n[gw]\npool.type = gateway\nlisten = {{ADDR[http]}}\n" .
        "http.gateways = 1\nhttp.route[web] = /\nhttp.sendfile_root = relative/dir\n" .
        "[web]\npool.type = fastcgi\nlisten = {{ADDR}}\npm = static\npm.max_children = 1",
    ["http.sendfile_root must be an absolute path, got 'relative/dir'"]
);

expectConfigFailure(
    'gateway-missing-root',
    "[global]\nerror_log = {{FILE:LOG}}\n[gw]\npool.type = gateway\nlisten = {{ADDR[http]}}\n" .
        "http.gateways = 1\nhttp.route[web] = /\nhttp.sendfile_root = /nonexistent/fpmng-649\n" .
        "[web]\npool.type = fastcgi\nlisten = {{ADDR}}\npm = static\npm.max_children = 1",
    ["http.sendfile_root '/nonexistent/fpmng-649' is not an existing directory"]
);

/* "/" resolves to a directory, but every path below it is refused by the
 * containment check, so the directive would serve nothing. The refusal is by
 * the resolved path, so "//" is refused as well. */
expectConfigFailure(
    'gateway-root-slash',
    "[global]\nerror_log = {{FILE:LOG}}\n[gw]\npool.type = gateway\nlisten = {{ADDR[http]}}\n" .
        "http.gateways = 1\nhttp.route[web] = /\nhttp.sendfile_root = //\n" .
        "[web]\npool.type = fastcgi\nlisten = {{ADDR}}\npm = static\npm.max_children = 1",
    ["http.sendfile_root must not be '/'"]
);
?>
--EXPECT--
fastcgi-sendfile-root: rejected
http-direct-sendfile-root: rejected
gateway-relative-root: rejected
gateway-missing-root: rejected
gateway-root-slash: rejected
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
