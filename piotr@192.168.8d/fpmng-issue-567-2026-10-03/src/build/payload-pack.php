<?php
/**
 * fpm-ng: append a typed payload to a finished binary (issue #171).
 *
 * The format is described in sapi/fpmng/fpm/fpm_payload.h (the payload record)
 * and sapi/fpmng/fpm/fpm_payload_dist.h (the archive inside a distribution
 * entry). This script is the only writer of both; the C side only reads.
 *
 * In PHP, not C, because appending is not compilation: NOTES.md:157-201 makes a
 * point of packaging needing no toolchain, and a build that just linked a
 * binary against libphp has a PHP interpreter at hand by definition. In shell
 * it would need SHA-256 and little-endian 64-bit integers out of od(1) and
 * printf(1), which is how this kind of script grows a bug nobody reads.
 *
 * Usage:
 *   payload-pack.php append --binary=PATH --kind=distribution --dir=DIR [--prefix=P]
 *   payload-pack.php list   --binary=PATH
 *   payload-pack.php digest --dir=DIR [--prefix=P]
 *
 * `digest` prints the SHA-256 of the archive `append` would build from the same
 * directory, without touching a binary. build/embed-payload.sh compares it with
 * what a binary already carries, which is how embedding stays idempotent on a
 * build tree that is reused between CI runs -- appending on every run would
 * grow the binary by one archive each time.
 */

const MAGIC = 'FPMNGPY1';
const RECORD_SIZE = 72;
const ARCHIVE_MAGIC = 'FPMNGAR1';
const KINDS = ['distribution' => 1, 'application' => 2];

function fail(string $message): never
{
    fwrite(STDERR, "payload-pack.php: $message\n");
    exit(1);
}

/** Little-endian, fixed width, matching the byte-by-byte parser in C. */
function u32(int $v): string { return pack('V', $v); }
function u64(int $v): string { return pack('P', $v); }

function options(array $argv): array
{
    $opts = [];
    foreach (array_slice($argv, 2) as $arg) {
        if (!preg_match('/^--([a-z-]+)=(.*)$/', $arg, $m)) {
            fail("unrecognised argument '$arg'");
        }
        $opts[$m[1]] = $m[2];
    }
    return $opts;
}

/**
 * Walks the record chain backwards from the end of the file, exactly as
 * fpm_payload_find() does, and returns the entries newest first.
 */
function records(string $binary): array
{
    if (!is_file($binary)) {
        fail("cannot read $binary");
    }
    $size = filesize($binary);
    if ($size === false || $size < RECORD_SIZE) {
        return [];
    }
    $fp = fopen($binary, 'rb') ?: fail("cannot read $binary");
    $at = $size - RECORD_SIZE;
    $out = [];
    while (count($out) < 64) {
        fseek($fp, $at);
        $record = fread($fp, RECORD_SIZE);
        if (strlen($record) !== RECORD_SIZE || substr($record, 0, 8) !== MAGIC) {
            break;
        }
        $entry = [
            'record_offset' => $at,
            'kind' => unpack('V', substr($record, 8, 4))[1],
            'offset' => unpack('P', substr($record, 16, 8))[1],
            'size' => unpack('P', substr($record, 24, 8))[1],
            'digest' => bin2hex(substr($record, 32, 32)),
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

/**
 * The archive: magic, count, then one index record per file (name length, data
 * size, data offset, name), then the data. Sorted by name so that the same
 * directory produces the same bytes on every machine -- a build that is not
 * reproducible cannot be checked against criterion 4 by digest.
 */
function archive(string $dir, string $prefix): string
{
    $files = [];
    $it = new RecursiveIteratorIterator(new RecursiveDirectoryIterator($dir, FilesystemIterator::SKIP_DOTS));
    foreach ($it as $file) {
        if (!$file->isFile()) {
            continue;
        }
        $name = substr($file->getPathname(), strlen(rtrim($dir, '/')) + 1);
        $files[($prefix !== '' ? $prefix . '/' : '') . $name] = file_get_contents($file->getPathname());
    }
    if (!$files) {
        fail("no files under $dir");
    }
    ksort($files);

    $index = '';
    $data = '';
    $header_size = strlen(ARCHIVE_MAGIC) + 4;
    foreach ($files as $name => $content) {
        $index .= u32(strlen($name)) . u64(strlen($content)) . u64(0) . $name;
    }
    /* Offsets are from the start of the archive and the index is variable
     * length, so its total size has to be known before any of them can be
     * written: build it once with zeros, then rewrite the offsets in place. */
    $at = $header_size + strlen($index);
    $index = '';
    foreach ($files as $name => $content) {
        $index .= u32(strlen($name)) . u64(strlen($content)) . u64($at) . $name;
        $at += strlen($content);
        $data .= $content;
    }
    return ARCHIVE_MAGIC . u32(count($files)) . $index . $data;
}

$command = $argv[1] ?? '';
$opts = options($argv);

if ($command === 'digest') {
    echo hash('sha256', archive($opts['dir'] ?? fail('--dir is required'), $opts['prefix'] ?? '')), "\n";
    exit(0);
}

$binary = $opts['binary'] ?? fail('--binary is required');

if ($command === 'list') {
    foreach (records($binary) as $r) {
        printf("kind=%d offset=%d size=%d sha256=%s\n", $r['kind'], $r['offset'], $r['size'], $r['digest']);
    }
    exit(0);
}
if ($command !== 'append') {
    fail("usage: payload-pack.php append|list|digest [...]");
}

$kind = KINDS[$opts['kind'] ?? ''] ?? fail('--kind must be distribution or application');
$payload = isset($opts['dir'])
    ? archive($opts['dir'], $opts['prefix'] ?? '')
    : file_get_contents($opts['file'] ?? fail('--dir or --file is required'));

$existing = records($binary);
$previous = $existing ? $existing[0]['record_offset'] : 0;
$offset = filesize($binary);

/* Append only: the binary is opened for appending and nothing already in it is
 * read back, let alone rewritten. That is what keeps an entry written by an
 * earlier run byte-identical (acceptance criterion 4). */
$fp = fopen($binary, 'ab') ?: fail("cannot append to $binary");
$record = MAGIC . u32($kind) . u32(0) . u64($offset) . u64(strlen($payload))
    . hash('sha256', $payload, true) . u64($previous);
if (strlen($record) !== RECORD_SIZE) {
    fail('internal error: the payload record is not ' . RECORD_SIZE . ' bytes');
}
fwrite($fp, $payload . $record);
fclose($fp);

printf("payload-pack.php: appended kind=%s size=%d at offset=%d sha256=%s\n",
    $opts['kind'], strlen($payload), $offset, hash('sha256', $payload));
