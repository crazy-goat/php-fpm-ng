--TEST--
fpm-ng: supplied php.ini settings are applied at PHP module startup, before fpm.conf is read; host ini/scan and -n/-c/-d/-y precedence pinned (issue #428)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #428, acceptance criterion 2. docs/NOTES.md section 3a says the host's
 * php.ini and conf.d are not silently merged with embedded inputs and that
 * -c, -d and --fpm-config are explicit overrides whose precedence "#428's
 * observable startup tests" settle. This is that test: it observes what the
 * binary does with `-i` (phpinfo as text, which runs the whole PHP startup and
 * starts no pool), so it pins behaviour that already exists in PHP's ini
 * loader and in fpm_main.c rather than something this change added.
 *
 * Why this is enough for the embedded php.ini: the SAPI's ini_entries string
 * (what -d fills) is parsed by php_init_config() inside php_module_startup(),
 * and `extension=` entries it contains are consumed by the same startup, before
 * any module's MINIT. fpm.conf is read later still, in fpm_init(). So an
 * embedded php.ini handed over as ini_entries plus php_ini_ignore (what -n sets)
 * is a startup-stage input by construction, and "-n -d ..." below is the same
 * code path driven from the command line.
 *
 * Cases marked CURRENT are behaviours of the loader that a packed executable
 * will need to decide about (see the outcome comment on the issue); they are
 * pinned here so a change to them is noticed, not because they are endorsed.
 */

function check(bool $condition, string $message): void
{
    if (!$condition) {
        echo "FAIL: $message\n";
        exit(1);
    }
}

$binary = FPM\Tester::findExecutable();
$dir = sys_get_temp_dir() . '/fpmng-428-ini-' . getmypid();
mkdir($dir);
mkdir("$dir/scan");
file_put_contents("$dir/main.ini", "memory_limit=11M\nprecision=5\n");
file_put_contents("$dir/scan/a.ini", "precision=6\nserialize_precision=7\n");

/** Runs the binary with a controlled ini environment. */
function run(string $binary, array $args, array $env = []): array
{
    $base = getenv();
    unset($base['PHP_INI_SCAN_DIR'], $base['PHPRC']);
    $proc = proc_open(array_merge([$binary], $args), [1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes, null, $env + $base);
    $out = stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    $status = proc_close($proc);
    return [$status, $out];
}

/** -i, reduced to the lines this test is about. */
function info(string $binary, array $args, array $env = []): string
{
    global $dir;
    [$status, $out] = run($binary, array_merge($args, ['-i']), $env);
    check($status === 0, "-i exited $status: $out");
    $keep = [];
    foreach (explode("\n", $out) as $line) {
        if (preg_match('/^(Loaded Configuration File|Scan this dir for additional \.ini files|Additional \.ini files parsed|memory_limit|precision|serialize_precision) => (.*)$/', $line, $m)) {
            $keep[] = $m[1] . '=' . str_replace($dir, 'DIR', preg_replace('/ => .*/', '', $m[2]));
        }
    }
    return implode("\n", $keep);
}

$host = ['PHP_INI_SCAN_DIR' => "$dir/scan", 'PHPRC' => $dir];

echo "== -n ignores the host: PHPRC and PHP_INI_SCAN_DIR set\n";
echo info($binary, ['-n'], $host), "\n";

echo "== -n -d: entries are the whole configuration\n";
echo info($binary, ['-n', '-d', 'memory_limit=77M', '-d', 'precision=3']), "\n";

echo "== CURRENT: -c reads its file, and the host's PHP_INI_SCAN_DIR is still merged in\n";
echo info($binary, ['-c', "$dir/main.ini"], $host), "\n";

echo "== -c, then -d: -d wins\n";
echo info($binary, ['-c', "$dir/main.ini", '-d', 'memory_limit=55M'], $host), "\n";

echo "== -n -c: -c still loads its file, and no scan dir is read\n";
echo info($binary, ['-n', '-c', "$dir/main.ini"], $host), "\n";

echo "== an empty PHP_INI_SCAN_DIR switches the scan off\n";
echo info($binary, ['-c', "$dir/main.ini"], ['PHP_INI_SCAN_DIR' => '']), "\n";

echo "== CURRENT: -c naming a missing file is silent, and the scan dir is still read\n";
echo info($binary, ['-c', "$dir/missing.ini"], $host), "\n";

echo "== php.ini is a startup input: extension= is consumed before fpm.conf is read\n";
$log = "$dir/startup.log";
[$status, $out] = run($binary, ['-n', '-d', "extension_dir=$dir", '-d', 'extension=nosuch428',
    '-d', 'log_errors=1', '-d', "error_log=$log", '-t', '-y', "$dir/no-such.conf"]);
check($status !== 0, 'a missing fpm.conf passed');
$startup = is_file($log) ? file_get_contents($log) : '';
check(str_contains($startup, "PHP Startup: Unable to load dynamic library 'nosuch428'"), "the extension= entry was not processed at startup:\n$startup\n$out");
check(str_contains($out, "failed to open configuration file '$dir/no-such.conf'"), "fpm.conf was not read after startup:\n$out");
echo "startup warning logged, then fpm.conf failure\n";

echo "== -y wins over php.ini's fpm.config\n";
[$status, $out] = run($binary, ['-n', '-d', "fpm.config=$dir/from-ini.conf", '-y', "$dir/from-y.conf", '-t']);
check(str_contains($out, "'$dir/from-y.conf'") && !str_contains($out, 'from-ini.conf'), "wrong config chosen:\n$out");
echo "-y\n";
[$status, $out] = run($binary, ['-n', '-d', "fpm.config=$dir/from-ini.conf", '-t']);
check(str_contains($out, "'$dir/from-ini.conf'"), "fpm.config was not used:\n$out");
echo "fpm.config\n";

echo "Done\n";
?>
--CLEAN--
<?php
$dir = sys_get_temp_dir() . '/fpmng-428-ini-' . getmypid();
foreach (array_merge(glob("$dir/scan/*") ?: [], glob("$dir/*") ?: []) as $f) {
    @unlink($f);
}
@rmdir("$dir/scan");
@rmdir($dir);
?>
--EXPECT--
== -n ignores the host: PHPRC and PHP_INI_SCAN_DIR set
Loaded Configuration File=(none)
Scan this dir for additional .ini files=(none)
Additional .ini files parsed=(none)
memory_limit=128M
precision=14
serialize_precision=-1
== -n -d: entries are the whole configuration
Loaded Configuration File=(none)
Scan this dir for additional .ini files=(none)
Additional .ini files parsed=(none)
memory_limit=77M
precision=3
serialize_precision=-1
== CURRENT: -c reads its file, and the host's PHP_INI_SCAN_DIR is still merged in
Loaded Configuration File=DIR/main.ini
Scan this dir for additional .ini files=DIR/scan
Additional .ini files parsed=DIR/scan/a.ini
memory_limit=11M
precision=6
serialize_precision=7
== -c, then -d: -d wins
Loaded Configuration File=DIR/main.ini
Scan this dir for additional .ini files=DIR/scan
Additional .ini files parsed=DIR/scan/a.ini
memory_limit=55M
precision=6
serialize_precision=7
== -n -c: -c still loads its file, and no scan dir is read
Loaded Configuration File=DIR/main.ini
Scan this dir for additional .ini files=(none)
Additional .ini files parsed=(none)
memory_limit=11M
precision=5
serialize_precision=-1
== an empty PHP_INI_SCAN_DIR switches the scan off
Loaded Configuration File=DIR/main.ini
Scan this dir for additional .ini files=(none)
Additional .ini files parsed=(none)
memory_limit=11M
precision=5
serialize_precision=-1
== CURRENT: -c naming a missing file is silent, and the scan dir is still read
Loaded Configuration File=(none)
Scan this dir for additional .ini files=DIR/scan
Additional .ini files parsed=DIR/scan/a.ini
memory_limit=128M
precision=6
serialize_precision=7
== php.ini is a startup input: extension= is consumed before fpm.conf is read
startup warning logged, then fpm.conf failure
== -y wins over php.ini's fpm.config
-y
fpm.config
Done
