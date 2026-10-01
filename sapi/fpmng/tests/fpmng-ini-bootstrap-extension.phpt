--TEST--
fpm-ng: a startup-only extension named by supplied ini entries is loaded at PHP startup (issue #428)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
/* Needs a shared extension built for this exact PHP: the stock CI build is
 * static (--disable-all) and has none, so there TEST_FPM_EXTENSION_DIR is
 * unset and this skips. Point it at a directory holding ctype.so built from the
 * same php-src with --enable-ctype=shared (issue #428 recorded the commands).
 * The policy test next to this one covers the stage on every build; this one
 * is the positive proof with a real module. */
$dir = getenv('TEST_FPM_EXTENSION_DIR');
if (!$dir || !is_file("$dir/ctype.so")) {
    die('skip TEST_FPM_EXTENSION_DIR does not hold ctype.so');
}
?>
--FILE--
<?php

require_once "tester.inc";

$binary = FPM\Tester::findExecutable();
$ext = getenv('TEST_FPM_EXTENSION_DIR');

function modules(string $binary, array $args): array
{
    $out = [];
    exec(escapeshellarg($binary) . ' ' . implode(' ', array_map('escapeshellarg', $args)) . ' -m 2>&1', $out);
    return $out;
}

/* Negative control: the same command line without the extension= entry. */
echo "without: ", in_array('ctype', modules($binary, ['-n', "-dextension_dir=$ext"]), true) ? 'loaded' : 'absent', "\n";

/* -n -d: the entries are the embedded-ini shape (ini_entries + php_ini_ignore). */
echo "-n -d: ", in_array('ctype', modules($binary, ['-n', "-dextension_dir=$ext", '-dextension=ctype']), true) ? 'loaded' : 'absent', "\n";

/* -c: the file is a main php.ini. */
$dir = sys_get_temp_dir() . '/fpmng-428-ext-' . getmypid();
mkdir($dir);
file_put_contents("$dir/php.ini", "extension_dir=$ext\nextension=ctype\n");
echo "-c: ", in_array('ctype', modules($binary, ['-c', "$dir/php.ini"]), true) ? 'loaded' : 'absent', "\n";
unlink("$dir/php.ini");
rmdir($dir);

echo "Done\n";
?>
--EXPECT--
without: absent
-n -d: loaded
-c: loaded
Done
