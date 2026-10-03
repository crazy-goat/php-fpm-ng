--TEST--
fpm-ng: fpm.conf diagnostics after an include= name the real line of the including file (issue #545)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #545. The parser's line counter is file-level state that loading the
 * included file restarts at 0; the lines that follow the include= in the outer
 * file, and the "Unable to include" message, must still use the outer file's
 * own numbers. The included files are longer than the outer position so a
 * counter left at the end of the included file is visible as a wrong number. */

$binary = FPM\Tester::findExecutable();
$dir = sys_get_temp_dir() . '/fpmng-545-include';
/* The CLEAN section runs in another process, so the name is fixed and a leftover from a killed run is removed here. */
foreach (glob("$dir/*") ?: [] as $leftover) {
    @unlink($leftover);
}
@rmdir($dir);
mkdir($dir);

function run(string $binary, string $file): array
{
    $proc = proc_open([$binary, '-n', '-t', '-y', $file], [1 => ['pipe', 'w'], 2 => ['redirect', 1]], $pipes);
    $out = stream_get_contents($pipes[1]);
    fclose($pipes[1]);
    $status = proc_close($proc);
    $lines = array_map(
        fn($l) => preg_replace('/^\[[^\]]*\] /', '', $l),
        array_values(array_filter(explode("\n", $out), 'strlen'))
    );
    return [$status, $lines];
}

/* Ten lines, all valid. */
file_put_contents("$dir/ok.inc", "[extra]\nlisten = $dir/extra.sock\npm = static\npm.max_children = 1\n"
    . "; 5\n; 6\n; 7\n; 8\n; 9\n; 10\n");
/* A bad directive on line 3. */
file_put_contents("$dir/bad.inc", "; 1\n; 2\nbogus_inner = 1\n; 4\n; 5\n; 6\n; 7\n");

/* 1. unknown entry on line 5 of the outer file, after an include on line 3. */
file_put_contents("$dir/outer.conf", "[global]\nerror_log = /dev/null\ninclude = $dir/ok.inc\n; 4\nbogus_outer = 1\n");
[$st, $out] = run($binary, "$dir/outer.conf");
check($st !== 0, 'a bad line after an include was accepted');
echo str_replace($dir, 'DIR', $out[0]), "\n";

/* 2. the include itself fails: the message names the include line. */
file_put_contents("$dir/outer2.conf", "[global]\nerror_log = /dev/null\n; 3\ninclude = $dir/bad.inc\n");
[$st, $out] = run($binary, "$dir/outer2.conf");
check($st !== 0, 'a bad included file was accepted');
foreach ($out as $line) {
    echo str_replace($dir, 'DIR', $line), "\n";
}

function check(bool $condition, string $message): void
{
    if (!$condition) {
        echo "FAIL: $message\n";
        exit(1);
    }
}
?>
--EXPECT--
ERROR: [DIR/outer.conf:5] unknown entry 'bogus_outer'
ERROR: [DIR/bad.inc:3] unknown entry 'bogus_inner'
ERROR: Unable to include DIR/bad.inc from DIR/outer2.conf at line 4
ERROR: failed to load configuration file 'DIR/outer2.conf'
ERROR: FPM initialization failed
--CLEAN--
<?php
$dir = sys_get_temp_dir() . '/fpmng-545-include';
foreach (glob("$dir/*") ?: [] as $f) {
    @unlink($f);
}
@rmdir($dir);
?>
