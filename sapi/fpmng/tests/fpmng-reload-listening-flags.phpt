--TEST--
fpm-ng: normalize fiber listening-socket flags across reloads
--SKIPIF--
<?php
include "fpmng-skipif.inc";
fpmng_skip_if_pool_type_unsupported('http');

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

$dir = __DIR__;
$port = (int) (getenv('FPMNG_TASK037_BASE_PORT') ?: 26037);
/* Two listeners, two ports: the pool's `listen` socket (required by every
 * request-serving type) and the gateway's `http.listen` cannot be the same
 * address -- the first bind wins and the second refuses to start. The flags
 * this test watches are the http listener's, because that is the socket the
 * fiber executor serves on. */
$listenAddress = "127.0.0.1:$port";
$address = "127.0.0.1:" . ($port + 1);

/* pool.type = http since issue #379: the retired pool type's fiber cells this
 * test was written on are gone with the type, and http x fiber is the
 * surviving fiber configuration. Both sides of the reload speak HTTP on
 * http.listen; a plain GET on the listening port carries the assertion the old
 * FastCGI request did. */
$fiberConfig = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 2
[reload]
listen = $listenAddress
http.listen = $address
chdir = $dir
pool.type = http
pool.executor = fiber
php_admin_value[opcache.enable] = 0
php_admin_value[max_execution_time] = 0
pm = static
pm.max_children = 1
EOT;

$classicConfig = <<<EOT
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 2
[reload]
listen = $listenAddress
http.listen = $address
chdir = $dir
pool.type = http
pm = static
pm.max_children = 1
EOT;

/* The gateway maps a request path onto a source file (same shape as the
 * fiber-matrix test): the script prints "ok", so the body check needs no
 * front controller. */
$httpCtx = stream_context_create(['http' => ['timeout' => 5, 'ignore_errors' => true]]);

$tester = new FPM\Tester($fiberConfig, '<?php echo "ok";');
$script = basename($tester->makeSourceFile('reload-flags-'));
$httpOk = function () use ($address, $script, $httpCtx): bool {
    return @file_get_contents("http://$address/$script", false, $httpCtx) === 'ok';
};

$tester->start();
$tester->expectLogStartNotices();
if (!$httpOk()) {
    throw new RuntimeException('fiber start did not answer over HTTP');
}
assertNonblocking(masterListenFlags($tester->getPid(), $address), true, 'fiber start');

$tester->reload($classicConfig);
$tester->expectLogReloadingNotices();
if (!$httpOk()) {
    throw new RuntimeException('classic after fiber reload did not answer over HTTP');
}
assertNonblocking(masterListenFlags($tester->getPid(), $address), false, 'classic after fiber reload');

$holdSeconds = (int) (getenv('FPMNG_TASK037_HOLD_SECONDS') ?: 0);
if ($holdSeconds > 0) {
    $deadline = microtime(true) + $holdSeconds;
    while (microtime(true) < $deadline) {
        if (!$httpOk()) {
            throw new RuntimeException('hold request did not answer over HTTP');
        }
        usleep(10000);
    }
}

$tester->reload($fiberConfig);
$tester->expectLogReloadingNotices();
if (!$httpOk()) {
    throw new RuntimeException('fiber after classic reload did not answer over HTTP');
}
assertNonblocking(masterListenFlags($tester->getPid(), $address), true, 'fiber after classic reload');

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
