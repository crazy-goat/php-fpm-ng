--TEST--
fpm-ng: the distribution payload embedded in the binary (issue #171)
--SKIPIF--
<?php
include "fpmng-skipif.inc";
/* The payload is the ACME client and nothing else, so a build without
 * --enable-fpmng-acme embeds none (issue #281) and there is nothing here to
 * test -- build/embed-payload.sh says so at build time, and
 * fpm_payload_dist_validate() says so at startup. */
fpmng_skip_if_no_acme();
?>
--FILE--
<?php

require_once "tester.inc";

/* Issue #171. The acceptance criteria that can be checked without a CA:
 *
 *   1. the format is readable by something other than its own writer
 *   2. entries are found by kind
 *   4. appending an application payload leaves the distribution entry
 *      byte-identical
 *   5. a pool can name an embedded script, and a name that is not there is a
 *      configuration error with a message that says so
 *   7. a binary with no payload at all still runs
 *
 * The reader below is written out in full rather than shelling out to
 * build/payload-pack.php (which is not in a prepared php-src tree anyway):
 * a format whose only reader is the code that wrote it has not been shown to
 * be a format. These bytes come from sapi/fpmng/fpm/fpm_payload.h.
 */

const RECORD = 72;
const MAGIC = 'FPMNGPY1';
const KIND_DISTRIBUTION = 1;
const KIND_APPLICATION = 2;

function check(bool $condition, string $message): void
{
    if (!$condition) {
        echo "FAIL: $message\n";
        exit(1);
    }
}

/** Walks the record chain backwards from the end of the file, newest first. */
function records(string $binary): array
{
    $size = filesize($binary);
    $fp = fopen($binary, 'rb');
    $at = $size - RECORD;
    $out = [];
    while ($at >= 0 && count($out) < 64) {
        fseek($fp, $at);
        $record = fread($fp, RECORD);
        if (strlen($record) !== RECORD || substr($record, 0, 8) !== MAGIC) {
            break;
        }
        $entry = [
            'kind' => unpack('V', substr($record, 8, 4))[1],
            'offset' => unpack('P', substr($record, 16, 8))[1],
            'size' => unpack('P', substr($record, 24, 8))[1],
            'digest' => substr($record, 32, 32),
            'prev' => unpack('P', substr($record, 64, 8))[1],
        ];
        $out[] = $entry;
        if ($entry['prev'] === 0 || $entry['prev'] >= $at) {
            break;
        }
        $at = $entry['prev'];
    }
    fclose($fp);
    return $out;
}

function entryBytes(string $binary, array $entry): string
{
    $fp = fopen($binary, 'rb');
    fseek($fp, $entry['offset']);
    $data = fread($fp, $entry['size']);
    fclose($fp);
    return $data;
}

/** The names in a distribution archive, parsed the same way fpm_payload_dist.c does. */
function members(string $archive): array
{
    check(substr($archive, 0, 8) === 'FPMNGAR1', 'the distribution payload is not an archive');
    $count = unpack('V', substr($archive, 8, 4))[1];
    $at = 12;
    $names = [];
    for ($i = 0; $i < $count; $i++) {
        $nameLen = unpack('V', substr($archive, $at, 4))[1];
        $size = unpack('P', substr($archive, $at + 4, 8))[1];
        $offset = unpack('P', substr($archive, $at + 12, 8))[1];
        $at += 20;
        $names[substr($archive, $at, $nameLen)] = substr($archive, $offset, $size);
        $at += $nameLen;
    }
    return $names;
}

/** Runs `-t` on a configuration with the given binary and returns its output. */
function configTest(string $binary, string $config): string
{
    $file = sys_get_temp_dir() . '/fpmng-payload-' . getmypid() . '.conf';
    file_put_contents($file, $config);
    $descriptors = [1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $proc = proc_open([$binary, '-n', '-y', $file, '-t'], $descriptors, $pipes);
    $out = stream_get_contents($pipes[1]) . stream_get_contents($pipes[2]);
    fclose($pipes[1]);
    fclose($pipes[2]);
    proc_close($proc);
    unlink($file);
    return $out;
}

function cronConfig(string $script): string
{
    return "[global]\nerror_log = /dev/null\n[p]\npool.type = cron\n"
        . "cron.schedule = * * * * *\ncron.script = $script\n";
}

$binary = realpath(FPM\Tester::findExecutable());
check($binary !== false, 'no fpm binary to inspect');

/* --- criteria 1 and 2: the entry is there and is found by kind ----------- */
$entries = records($binary);
$distribution = null;
foreach ($entries as $entry) {
    if ($entry['kind'] === KIND_DISTRIBUTION) {
        $distribution = $entry;
        break;
    }
}
check($distribution !== null, 'the binary carries no distribution payload');
$archive = entryBytes($binary, $distribution);
check(hash('sha256', $archive, true) === $distribution['digest'],
    'the distribution payload does not match the digest in its record');
$members = members($archive);
check(isset($members['acme/renew.php']), 'acme/renew.php is not in the distribution payload');
check(str_contains($members['acme/renew.php'], 'namespace FpmNg\\Acme'),
    'the embedded acme/renew.php is not the file this repository ships');
echo "payload: acme/renew.php embedded, digest verified\n";

/* --- criterion 5: a pool names an embedded script ------------------------ */
$out = configTest($binary, cronConfig('fpmng-dist://acme/renew.php'));
check(str_contains($out, 'successful'), "an embedded cron.script was rejected:\n$out");
echo "config: cron.script = fpmng-dist://acme/renew.php accepted\n";

$out = configTest($binary, cronConfig('fpmng-dist://acme/not-shipped.php'));
check(str_contains($out, 'no such file in the embedded distribution payload'),
    "a missing embedded file was not reported as one:\n$out");
echo "config: a name that is not embedded is refused at startup\n";

/* --- criterion 4: appending an application payload changes nothing ------- */
$copy = sys_get_temp_dir() . '/fpmng-payload-copy-' . getmypid();
check(copy($binary, $copy), 'cannot copy the binary');
chmod($copy, 0700);

$application = 'an application payload, contents irrelevant to this test';
$offset = filesize($copy);
$record = MAGIC . pack('V', KIND_APPLICATION) . pack('V', 0) . pack('P', $offset)
    . pack('P', strlen($application)) . hash('sha256', $application, true)
    . pack('P', $entries[0]['offset'] + $entries[0]['size']);
file_put_contents($copy, $application . $record, FILE_APPEND);

$after = records($copy);
check(count($after) === count($entries) + 1, 'the appended entry did not join the chain');
check($after[0]['kind'] === KIND_APPLICATION, 'the newest entry is not the one just appended');
$distributionAfter = null;
foreach ($after as $entry) {
    if ($entry['kind'] === KIND_DISTRIBUTION) {
        $distributionAfter = $entry;
        break;
    }
}
check($distributionAfter == $distribution, 'the distribution record changed when an application was appended');
check(entryBytes($copy, $distributionAfter) === $archive,
    'the distribution payload changed when an application was appended');
$out = configTest($copy, cronConfig('fpmng-dist://acme/renew.php'));
check(str_contains($out, 'successful'), "the repacked binary lost its embedded script:\n$out");
echo "repack: an application payload leaves the distribution entry byte-identical\n";

/* --- criterion 7: a binary with no payload still runs -------------------- */
$bare = sys_get_temp_dir() . '/fpmng-payload-bare-' . getmypid();
$fp = fopen($binary, 'rb');
/* Everything before the first payload is the binary as the linker produced it:
 * the payloads are appended after the end of the ELF image and nothing in it
 * refers to them. */
file_put_contents($bare, fread($fp, $distribution['offset']));
fclose($fp);
chmod($bare, 0700);
check(records($bare) === [], 'the truncated binary still reports a payload');

$script = sys_get_temp_dir() . '/fpmng-payload-' . getmypid() . '.php';
file_put_contents($script, "<?php\n");
$out = configTest($bare, cronConfig($script));
check(str_contains($out, 'successful'), "a binary with no payload cannot run at all:\n$out");
$out = configTest($bare, cronConfig('fpmng-dist://acme/renew.php'));
check(str_contains($out, 'carries no embedded distribution payload'),
    "a binary with no payload did not say so:\n$out");
echo "unpacked: a binary with no payload runs, and says so when asked for one\n";

unlink($copy);
unlink($bare);
unlink($script);
?>
--EXPECT--
payload: acme/renew.php embedded, digest verified
config: cron.script = fpmng-dist://acme/renew.php accepted
config: a name that is not embedded is refused at startup
repack: an application payload leaves the distribution entry byte-identical
unpacked: a binary with no payload runs, and says so when asked for one
