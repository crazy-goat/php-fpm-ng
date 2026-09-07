--TEST--
fpm-ng: a fiber pool does not change an unrelated classic socket
--SKIPIF--
<?php
include "skipif.inc";

if (PHP_OS_FAMILY !== 'Linux' || !is_dir('/proc')) {
    die('skip requires Linux /proc socket metadata');
}

$binary = getenv('TEST_PHP_FPM_EXECUTABLE') ?: FPM\Tester::findExecutable();
exec(escapeshellarg($binary) . ' -i 2>&1', $output, $status);
if ($status !== 0 || !str_contains(implode("\n", $output), '--enable-fpmng-fiber')) {
    die('skip php-fpm-ng was not built with --enable-fpmng-fiber');
}
?>
--FILE--
<?php
require_once "tester.inc";

function masterListenFlags(int $pid, string $address): int
{
    $separator = strrpos($address, ':');
    if ($separator === false) {
        throw new RuntimeException("cannot parse listen address: $address");
    }

    $port = (int) substr($address, $separator + 1);
    $portHex = strtoupper(str_pad(dechex($port), 4, '0', STR_PAD_LEFT));
    $inode = null;

    foreach (["/proc/$pid/net/tcp", "/proc/$pid/net/tcp6"] as $table) {
        if (!is_readable($table)) {
            continue;
        }
        foreach (file($table, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $line) {
            $fields = preg_split('/\s+/', trim($line));
            if (count($fields) < 10 || str_starts_with($fields[0], 'sl')) {
                continue;
            }
            $local = strtoupper($fields[1]);
            if ($fields[3] === '0A' && str_ends_with($local, ":$portHex")) {
                $inode = $fields[9];
                break 2;
            }
        }
    }

    if ($inode === null) {
        throw new RuntimeException("listening socket not found for $address");
    }

    foreach (glob("/proc/$pid/fd/*") as $fdPath) {
        if (@readlink($fdPath) !== "socket:[$inode]") {
            continue;
        }
        $fdInfo = @file_get_contents('/proc/' . $pid . '/fdinfo/' . basename($fdPath));
        if ($fdInfo !== false && preg_match('/^flags:\s*([0-7]+)/m', $fdInfo, $match)) {
            return (int) octdec($match[1]);
        }
    }

    throw new RuntimeException("master fd for $address not found");
}

function assertNonblocking(int $flags, bool $expected, string $label): void
{
    /* Linux reports O_NONBLOCK as octal 04000 in /proc fdinfo flags. */
    $actual = ($flags & 04000) !== 0;
    if ($actual !== $expected) {
        throw new RuntimeException(sprintf(
            '%s: expected O_NONBLOCK=%s, got flags %o',
            $label,
            $expected ? 'on' : 'off',
            $flags,
        ));
    }
}

$basePort = (int) (getenv('FPMNG_TASK037_BASE_PORT') ?: 26039);
$fiberAddress = "127.0.0.1:$basePort";
$classicAddress = '127.0.0.1:' . ($basePort + 1);

$config = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[fiber]
listen = $fiberAddress
pool.type = fastcgi-ng
pool.executor = fiber
php_admin_value[opcache.enable] = 0
php_admin_value[max_execution_time] = 0
pm = static
pm.max_children = 1
[classic]
listen = $classicAddress
pm = static
pm.max_children = 1
EOT;

$tester = new FPM\Tester($config, '<?php echo "ok";');

$tester->start();
$tester->expectLogStartNotices();
$tester->request(address: $fiberAddress)->expectBody('ok', skipHeadersCheck: true);
$tester->request(address: $classicAddress)->expectBody('ok', skipHeadersCheck: true);
assertNonblocking(masterListenFlags($tester->getPid(), $fiberAddress), true, 'fiber socket');
assertNonblocking(masterListenFlags($tester->getPid(), $classicAddress), false, 'unrelated classic socket');

$tester->terminate();
$tester->expectLogTerminatingNotices();
$tester->close();

?>
Done
--EXPECT--
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
?>
