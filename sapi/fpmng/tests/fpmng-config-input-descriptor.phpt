--TEST--
fpm-ng: fpm.conf from a non-file source gives the same settings as the file, names its input in diagnostics and refuses include= (issue #428)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #428. The configuration parser takes text, not a path: a file is read
 * into memory and handed to fpm_conf_load_ini_buffer(), and `-y fd:N` hands it
 * the bytes of an open descriptor (a pipe here, so there is no file anywhere
 * on disk that holds them). That is the interface the embedded application
 * payload will use, minus the payload, which is not activated yet.
 *
 * What is pinned:
 *   1. the -tt dump of a file and of the same bytes through a descriptor are
 *      identical, line for line, except for the name in the closing line;
 *   2. a syntax error names its input -- the path for a file (the negative
 *      control: ordinary behaviour), "fd:3" for a descriptor -- and the line;
 *   3. include= works in a file and is refused, naming the input, in anything
 *      that is not one (docs/NOTES.md section 3a: no relative resolution
 *      against the host's working directory);
 *   4. a descriptor cannot drive a running master (it cannot be re-read on
 *      reload), and malformed spellings are refused.
 */

function check(bool $condition, string $message): void
{
    if (!$condition) {
        echo "FAIL: $message\n";
        exit(1);
    }
}

$binary = FPM\Tester::findExecutable();
$dir = sys_get_temp_dir() . '/fpmng-428-input';
/* The CLEAN section runs in another process, so the name is fixed (a pid in it would never match) and a leftover from a killed run is removed here. */
foreach (array_merge(glob("$dir/scan/*") ?: [], glob("$dir/*") ?: []) as $leftover) {
    @unlink($leftover);
}
@rmdir("$dir/scan");
@rmdir($dir);
mkdir($dir);

/** Runs the binary; $config goes to fd 3 through a pipe when not null. */
function run(string $binary, array $args, ?string $config): array
{
    $spec = [1 => ['pipe', 'w'], 2 => ['redirect', 1]];
    if ($config !== null) {
        $spec[3] = ['pipe', 'r'];
    }
    $proc = proc_open(array_merge([$binary, '-n'], $args), $spec, $pipes);
    if ($config !== null) {
        fwrite($pipes[3], $config);
        fclose($pipes[3]);
    }
    $out = stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    $status = proc_close($proc);
    /* Drop the timestamp; keep the level and the message. */
    $lines = array_map(
        fn($l) => preg_replace('/^\[[^\]]*\] /', '', $l),
        array_values(array_filter(explode("\n", $out), 'strlen'))
    );
    return [$status, $lines];
}

touch("$dir/job.php");
$conf = <<<EOT
[global]
error_log = /dev/null
pid = $dir/m.pid

[web]
listen = $dir/web.sock
pm = dynamic
pm.max_children = 4
pm.start_servers = 1
pm.min_spare_servers = 1
pm.max_spare_servers = 2
env[FOO] = bar
php_admin_value[memory_limit] = 64M

[job]
pool.type = cron
cron.schedule = @daily
cron.script = $dir/job.php
EOT;
file_put_contents("$dir/ok.conf", $conf . "\n");

/* 1. same effective settings */
[$st1, $fromFile] = run($binary, ['-tt', '-y', "$dir/ok.conf"], null);
[$st2, $fromFd] = run($binary, ['-tt', '-y', 'fd:3'], $conf . "\n");
check($st1 === 0 && $st2 === 0, "-tt failed: file=$st1 descriptor=$st2\n" . implode("\n", $fromFd));
check(count($fromFile) > 60, 'the dump is suspiciously short: ' . count($fromFile));
$wanted = ['NOTICE: [web]', "NOTICE: \tpm.max_children = 4", "NOTICE: \tenv[FOO] = bar",
    "NOTICE: \tphp_admin_value[memory_limit] = 64M", 'NOTICE: [job]'];
foreach ($wanted as $line) {
    check(in_array($line, $fromFd, true), "descriptor dump lacks: $line");
}
$last = array_pop($fromFile);
$lastFd = array_pop($fromFd);
check($fromFile === $fromFd, 'file and descriptor dumps differ');
echo str_replace($dir, 'DIR', $last), "\n", $lastFd, "\n";

/* 2. diagnostics name the input */
$bad = "[global]\nerror_log = /dev/null\n\n[p]\nlisten = $dir/p.sock\nbogus = 1\n";
file_put_contents("$dir/bad.conf", $bad);
[$st, $out] = run($binary, ['-t', '-y', "$dir/bad.conf"], null);
check($st !== 0, 'a bad file was accepted');
echo str_replace($dir, 'DIR', $out[0]), "\n";
[$st, $out] = run($binary, ['-t', '-y', 'fd:3'], $bad);
check($st !== 0, 'a bad descriptor was accepted');
echo $out[0], "\n";

/* 3. include= */
file_put_contents("$dir/inc.conf", "[extra]\nlisten = $dir/extra.sock\npm = static\npm.max_children = 1\n");
$withInclude = "[global]\nerror_log = /dev/null\ninclude = $dir/inc.conf\n";
file_put_contents("$dir/outer.conf", $withInclude);
[$st, $out] = run($binary, ['-tt', '-y', "$dir/outer.conf"], null);
check($st === 0 && in_array('NOTICE: [extra]', $out, true), 'include from a file stopped working');
echo "file include: ok\n";
[$st, $out] = run($binary, ['-t', '-y', 'fd:3'], $withInclude);
check($st !== 0, 'include from a descriptor was accepted');
echo str_replace($dir, 'DIR', $out[0]), "\n";

/* 4. only with -t; malformed spellings */
[$st, $out] = run($binary, ['-y', 'fd:3'], $conf . "\n");
check($st !== 0, 'a descriptor drove a non-test start');
echo $out[0], "\n";
foreach (['fd:2', 'fd:x', 'fd:', 'fd:3x'] as $spec) {
    [$st, $out] = run($binary, ['-t', '-y', $spec], null);
    check($st !== 0, "$spec was accepted");
    echo $out[0], "\n";
}
[$st, $out] = run($binary, ['-t', '-y', 'fd:99'], null);
check($st !== 0 && str_contains(implode("\n", $out), "'fd:99'"), 'a closed descriptor was accepted or unnamed');
echo "closed descriptor: refused, named\n";

echo "Done\n";
?>
--CLEAN--
<?php
$dir = sys_get_temp_dir() . '/fpmng-428-input';
foreach (glob("$dir/*") ?: [] as $f) {
    @unlink($f);
}
@rmdir($dir);
?>
--EXPECT--
NOTICE: configuration file DIR/ok.conf test is successful
NOTICE: configuration file fd:3 test is successful
ERROR: [DIR/bad.conf:6] unknown entry 'bogus'
ERROR: [fd:3:6] unknown entry 'bogus'
file include: ok
ERROR: [fd:3:3] include = DIR/inc.conf: this configuration is not a file, so an include has nothing to resolve against; put the settings in the input itself
ERROR: 'fd:3': configuration from a descriptor is only accepted together with -t, because it cannot be read again on reload
ERROR: 'fd:2': a configuration descriptor is spelled fd:N with N >= 3
ERROR: 'fd:x': a configuration descriptor is spelled fd:N with N >= 3
ERROR: 'fd:': a configuration descriptor is spelled fd:N with N >= 3
ERROR: 'fd:3x': a configuration descriptor is spelled fd:N with N >= 3
closed descriptor: refused, named
Done
