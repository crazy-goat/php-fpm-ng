--TEST--
fpm-ng: `php-fpm-ng pack` embeds a PHAR, a php.ini and an fpm.conf as an application payload and refuses bad input (issue #429)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
if (PHP_OS_FAMILY !== 'Linux') die('skip requires Linux');
?>
--FILE--
<?php

require_once "tester.inc";
require_once "fpmng-pack-app.inc";

/* Issue #429. `pack` is a subcommand of the binary, so the test runs the real
 * binary on three small files and then reads the result back with a reader
 * written out here (the record layout is in sapi/fpmng/fpm/fpm_payload.h), not
 * with the code that wrote it. Checked:
 *   - the output starts with the exact bytes of the packing binary (the
 *     distribution payload, if any, stays byte-identical) and ends in a kind-2
 *     record whose SHA-256 matches its data;
 *   - the archive holds the three inputs unchanged under their entry names;
 *   - the output is a working php-fpm-ng after the inputs are deleted;
 *   - the PHAR stub is not run (it would create a marker file);
 *   - a missing, empty or wrong input, a second pack from the output, an
 *     existing output and a bad command line are refused with a message, exit
 *     status non-zero, and leave nothing at the output path. */

function check(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}
set_exception_handler(function (Throwable $e): void {
    echo 'FAIL: ' . $e->getMessage() . "\n";
});

function rmtree(string $dir): void
{
    foreach (glob("$dir/{,.}*", GLOB_BRACE) ?: [] as $f) {
        if (in_array(basename($f), ['.', '..'], true)) {
            continue;
        }
        is_dir($f) && !is_link($f) ? rmtree($f) : @unlink($f);
    }
    @rmdir($dir);
}

/** Runs the binary with $args; returns [exit status, stdout + stderr]. */
function run(string $binary, array $args): array
{
    $cmd = escapeshellarg($binary) . ' ' . implode(' ', array_map('escapeshellarg', $args)) . ' 2>&1';
    exec($cmd, $lines, $status);
    return [$status, implode("\n", $lines)];
}

/** Newest-first records of a binary: kind, data offset, size, digest, record offset. */
function records(string $binary): array
{
    $size = filesize($binary);
    $fp = fopen($binary, 'rb');
    $at = $size - 72;
    $out = [];
    while ($at >= 0 && count($out) < 8) {
        fseek($fp, $at);
        $r = fread($fp, 72);
        if (substr($r, 0, 8) !== 'FPMNGPY1') {
            break;
        }
        $out[] = [
            'kind' => unpack('V', substr($r, 8, 4))[1],
            'offset' => unpack('P', substr($r, 16, 8))[1],
            'size' => unpack('P', substr($r, 24, 8))[1],
            'digest' => substr($r, 32, 32),
            'record' => $at,
        ];
        $prev = unpack('P', substr($r, 64, 8))[1];
        if ($prev === 0) {
            break;
        }
        $at = $prev;
    }
    fclose($fp);
    return $out;
}

/** name => bytes of an FPMNGAR1 archive. */
function archive_entries(string $data): array
{
    check(substr($data, 0, 8) === 'FPMNGAR1', 'archive magic');
    $count = unpack('V', substr($data, 8, 4))[1];
    $p = 12;
    $out = [];
    for ($i = 0; $i < $count; $i++) {
        $nl = unpack('V', substr($data, $p, 4))[1];
        $size = unpack('P', substr($data, $p + 4, 8))[1];
        $off = unpack('P', substr($data, $p + 12, 8))[1];
        $out[substr($data, $p + 20, $nl)] = substr($data, $off, $size);
        $p += 20 + $nl;
    }
    return $out;
}

$binary = FPM\Tester::findExecutable();
$work = sys_get_temp_dir() . '/fpmng-429-pack';
rmtree($work);
mkdir($work, 0777, true);

$phar = "$work/app.phar";
$ini = "$work/php.ini";
$conf = "$work/fpm.conf";
$marker = "$work/stub-ran";
$phar_bytes = fpmng_mini_phar(['index.php' => "<?php echo 1;\n"], "<?php\nfile_put_contents(" . var_export($marker, true) . ", 'ran');\n__HALT_COMPILER(); ?>\r\n");
$ini_bytes = "; embedded\nmemory_limit = 64M\n";
$conf_bytes = "[global]\ndaemonize = no\nerror_log = /dev/stderr\n";
file_put_contents($phar, $phar_bytes);
file_put_contents($ini, $ini_bytes);
file_put_contents($conf, $conf_bytes);

try {
    $out = "$work/packed";
    [$status, $text] = run($binary, ['pack', $phar, $ini, $conf, '-o', $out]);
    check($status === 0, "pack failed ($status): $text");
    check(file_exists($out) && is_executable($out), 'the output is an executable file');
    check(!file_exists($marker), 'the PHAR stub ran while packing');
    check(glob("$work/packed.tmp.*") === [], 'a temporary file was left behind');

    $before = records($binary);
    $after = records($out);
    check(count($after) === count($before) + 1, 'one record more than the packing binary');
    check($after[0]['kind'] === 2, 'newest record is the application payload');
    if ($before) {
        check($after[1] === $before[0], 'the distribution record is unchanged');
    }
    $prefix = filesize($binary);
    check(hash_file('sha256', $out, false) !== hash_file('sha256', $binary, false), 'output differs from the binary');
    $a = fopen($binary, 'rb');
    $b = fopen($out, 'rb');
    $same = true;
    for ($n = 0; $n < $prefix; $n += 1 << 20) {
        $len = min(1 << 20, $prefix - $n);
        if (fread($a, $len) !== fread($b, $len)) {
            $same = false;
            break;
        }
    }
    check($same, 'the output does not start with the exact bytes of the packing binary');
    $data = file_get_contents($out, false, null, $after[0]['offset'], $after[0]['size']);
    check(hash('sha256', $data, true) === $after[0]['digest'], 'the digest in the record matches the data');
    $entries = archive_entries($data);
    check(array_keys($entries) === ['fpm.conf', 'php.ini', 'app.phar'], 'entry names: ' . implode(',', array_keys($entries)));
    check($entries['app.phar'] === $phar_bytes, 'the PHAR is stored unchanged');
    check($entries['php.ini'] === $ini_bytes, 'php.ini is stored unchanged');
    check($entries['fpm.conf'] === $conf_bytes, 'fpm.conf is stored unchanged');
    echo "packed\n";

    /* Same inputs, same bytes. */
    $out2 = "$work/packed2";
    [$status] = run($binary, ['pack', '--output=' . $out2, $phar, $ini, $conf]);
    check($status === 0, 'the --output= spelling works');
    check(hash_file('sha256', $out) === hash_file('sha256', $out2), 'packing twice is not reproducible');
    unlink($out2);

    /* The inputs are gone; the output still starts. */
    unlink($phar);
    unlink($ini);
    unlink($conf);
    [$status, $text] = run($out, ['-v']);
    check($status === 0 && stripos($text, 'PHP') !== false, "the packed executable does not run: $text");
    echo "runs\n";

    /* Refusals. Each must say something, exit non-zero and leave no output. */
    file_put_contents($phar, $phar_bytes);
    file_put_contents($ini, $ini_bytes);
    file_put_contents($conf, $conf_bytes);
    file_put_contents("$work/empty", '');
    file_put_contents("$work/plain.txt", "not a phar\n");
    file_put_contents("$work/nul.ini", "a=1\0b=2\n");
    mkdir("$work/dir");
    $cases = [
        'missing PHAR' => [["$work/none.phar", $ini, $conf], 'none.phar'],
        'missing php.ini' => [[$phar, "$work/none.ini", $conf], 'none.ini'],
        'missing fpm.conf' => [[$phar, $ini, "$work/none.conf"], 'none.conf'],
        'empty fpm.conf' => [[$phar, $ini, "$work/empty"], 'empty'],
        'not a PHAR' => [["$work/plain.txt", $ini, $conf], 'not a PHAR'],
        'NUL in php.ini' => [[$phar, "$work/nul.ini", $conf], 'NUL'],
        'directory as input' => [[$phar, $ini, "$work/dir"], 'not a regular file'],
    ];
    foreach ($cases as $name => [$inputs, $needle]) {
        $o = "$work/refused-" . md5($name);
        [$status, $text] = run($binary, array_merge(['pack'], $inputs, ['-o', $o]));
        check($status !== 0, "$name: exit status 0");
        check(str_contains($text, $needle), "$name: message lacks '$needle': $text");
        check(!file_exists($o) && glob("$o.tmp.*") === [], "$name: left a file at the output");
    }

    $out_hash = hash_file('sha256', $out);
    [$status, $text] = run($binary, ['pack', $phar, $ini, $conf, '-o', $out]);
    check($status !== 0 && str_contains($text, 'already exists'), "existing output: $text");
    check(hash_file('sha256', $out) === $out_hash, 'existing output was modified');

    /* -o naming an input or the packing binary is "exists" too, and neither file may change. */
    $hash_binary = hash_file('sha256', $binary);
    $hash_conf = hash_file('sha256', $conf);
    [$status, $text] = run($binary, ['pack', $phar, $ini, $conf, '-o', $conf]);
    check($status !== 0 && str_contains($text, 'already exists'), "-o equal to an input: $text");
    check(hash_file('sha256', $conf) === $hash_conf, '-o equal to an input changed the input');
    [$status, $text] = run($binary, ['pack', $phar, $ini, $conf, '-o', $binary]);
    check($status !== 0 && str_contains($text, 'already exists'), "-o equal to the binary: $text");
    check(hash_file('sha256', $binary) === $hash_binary, '-o equal to the binary changed the binary');

    /* Damaged artefacts. One flipped byte in the application data fails the digest; a truncated
     * file loses its record, so there is no payload to find; a record that describes data outside
     * the file is refused by `pack` itself. */
    $good = file_get_contents($out);
    $rec = records($out)[0];
    $flip = $rec['offset'] + 20;
    $bad = $good;
    $bad[$flip] = chr(ord($bad[$flip]) ^ 1);
    file_put_contents("$work/flipped", $bad);
    $r = records("$work/flipped")[0];
    check(hash('sha256', file_get_contents("$work/flipped", false, null, $r['offset'], $r['size']), true) !== $r['digest'], 'a flipped byte kept the digest valid');
    file_put_contents("$work/truncated", substr($good, 0, -5));
    check(records("$work/truncated") === [] || records("$work/truncated")[0]['kind'] !== 2, 'a truncated file still shows the application payload');
    $bad = $good;
    $bad[strlen($bad) - 72 + 31] = "\x7f";
    file_put_contents("$work/oversize", $bad);
    chmod("$work/oversize", 0755);
    [$status, $text] = run("$work/oversize", ['pack', $phar, $ini, $conf, '-o', "$work/from-damaged"]);
    check($status !== 0 && str_contains($text, 'broken') && !file_exists("$work/from-damaged"), "pack from a damaged record: $text");

    [$status, $text] = run($out, ['pack', $phar, $ini, $conf, '-o', "$work/again"]);
    check($status !== 0 && str_contains($text, 'already carries an application'), "repack: $text");
    check(!file_exists("$work/again"), 'repack left an output');

    [$status, $text] = run($binary, ['pack', $phar, $ini, '-o', "$work/two"]);
    check($status === 64 && str_contains($text, 'mandatory'), "two inputs: $status $text");
    [$status, $text] = run($binary, ['pack', $phar, $ini, $conf]);
    check($status === 64, "no -o: $status");
    [$status, $text] = run($binary, ['pack', '--bogus']);
    check($status === 64 && str_contains($text, 'unknown option'), "unknown option: $text");
    [$status, $text] = run($binary, ['pack', '--help']);
    check($status === 0 && str_contains($text, 'Usage: php-fpm-ng pack'), 'help');
    echo "refused\n";
} finally {
    rmtree($work);
}
?>
--EXPECT--
packed
runs
refused
