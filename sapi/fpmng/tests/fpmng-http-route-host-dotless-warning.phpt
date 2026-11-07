--TEST--
fpm-ng: a dot-less host route passes -t with a WARNING that names the entry and suggests the path prefix (issue #1070)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('gateway');
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #1070. A host-bound http.route[] entry with a dot-less name passes the
 * grammar of issue #658 and can name a real private-network host, so `-t`
 * accepts it: the WARNING says so without refusing, names the entry and points
 * at the path prefix the entry usually was meant to be ("http.route[api] = api"
 * for "/api"). 'localhost' and a multi-label name get no warning. */

function check(bool $condition, string $message): void
{
    if (!$condition) {
        echo "FAIL: $message\n";
        exit(1);
    }
}

$binary = FPM\Tester::findExecutable();
$dir = sys_get_temp_dir() . '/fpmng-1070-host-warning';
/* The CLEAN section runs in another process, so the name is fixed (a pid in it would never match) and a leftover from a killed run is removed here. */
foreach (glob("$dir/*") ?: [] as $leftover) {
    @unlink($leftover);
}
@rmdir($dir);
mkdir($dir);

/** Runs `-t` on a configuration and returns [exit code, zlog lines without the timestamps]. */
function runTest(string $binary, string $config, string $dir): array
{
    $file = "$dir/t.conf";
    file_put_contents($file, $config);
    exec($binary . ' -n -t -y ' . escapeshellarg($file) . ' 2>&1', $output, $code);
    $lines = array_map(
        fn($l) => preg_replace('/^\[[^\]]*\] /', '', $l),
        array_values(array_filter($output, 'strlen'))
    );
    return [$code, $lines];
}

function gateway(string $routeValue, string $dir): string
{
    return <<<EOT
[global]
error_log = /dev/null

[gw]
pool.type = gateway
listen = $dir/gw.sock
http.route[web] = /
http.route[site] = $routeValue

[web]
pool.type = fastcgi
listen = $dir/web.sock
pm = static
pm.max_children = 1

[site]
pool.type = fastcgi
listen = $dir/site.sock
pm = static
pm.max_children = 1
EOT;
}

/* The typo of the issue: a warning that names the entry and suggests '/api',
 * while the configuration itself stays accepted. */
[$code, $lines] = runTest($binary, gateway('api', $dir), $dir);
check($code === 0, "the dot-less route was refused:\n" . implode("\n", $lines));
$warnings = array_values(preg_grep('/^WARNING: \[pool gw\] http\.route\[site\]/', $lines));
check(count($warnings) === 1, "expected exactly one WARNING, got:\n" . implode("\n", $lines));
check(str_contains($warnings[0], "'api'"), "the WARNING does not name the entry: " . $warnings[0]);
check(str_contains($warnings[0], "'/api'"), "the WARNING does not suggest '/api': " . $warnings[0]);
echo "dotless host: warned\n";

[$code, $lines] = runTest($binary, gateway('localhost/', $dir), $dir);
check($code === 0, "the localhost route was refused:\n" . implode("\n", $lines));
check(!preg_grep('/dot-less/', $lines), "localhost got a WARNING:\n" . implode("\n", $lines));
echo "localhost: no warning\n";

[$code, $lines] = runTest($binary, gateway('a.example/', $dir), $dir);
check($code === 0, "the dotted route was refused:\n" . implode("\n", $lines));
check(!preg_grep('/dot-less/', $lines), "a dotted host got a WARNING:\n" . implode("\n", $lines));
echo "dotted host: no warning\n";

?>
Done
--CLEAN--
<?php
$dir = sys_get_temp_dir() . '/fpmng-1070-host-warning';
foreach (glob("$dir/*") ?: [] as $f) {
    @unlink($f);
}
@rmdir($dir);
?>
--EXPECT--
dotless host: warned
localhost: no warning
dotted host: no warning
Done
