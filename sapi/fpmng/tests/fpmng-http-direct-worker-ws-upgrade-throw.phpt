--TEST--
fpm-ng: a userland throw after fpmng_worker_upgrade() flushes the 101 and names the failed request (issue #461)
--SKIPIF--
<?php
include "skipif.inc";
?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$root = sys_get_temp_dir() . '/fpmng-worker-ws-upgrade-throw-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        if (($env['REQUEST_URI'] ?? '') === '/throw-after-upgrade-retained') {
            $GLOBALS['retained_ws'] = fpmng_worker_upgrade($id, []);
        } elseif (($env['REQUEST_URI'] ?? '') === '/return-after-upgrade-retained') {
            $GLOBALS['retained_ws'] = fpmng_worker_upgrade($id, []);
            /* Status-zero unexpected return is also an abnormal worker exit,
             * but it did not follow a userland exception. */
            exit(0);
        } else {
            /* The other half of #461: an uncaught throw unwinds this local
             * stream before the worker reaches its normal output-drain path. */
            $ws = fpmng_worker_upgrade($id, []);
        }
        throw new RuntimeException('deliberate throw after upgrade');
    }
});
fpmng_worker_event_enable($watcher);

while (true) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_WS_UPGRADE_THROW_PORT') ?: 28159);
$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[worker]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
catch_workers_output = no
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
php_admin_value[log_errors] = 0
CFG;

function readUpgradedResponse(int $port, string $path, string $key): string
{
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    check((bool) $fp, "connect failed: $error");
    stream_set_timeout($fp, 5);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: t\r\n"
        . "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        . "Sec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\n\r\n");
    /* EOF on the read side must not truncate output: the peer half-closes its
     * writer but still has to receive the 101 queued by the upgrade. */
    stream_socket_shutdown($fp, STREAM_SHUT_WR);

    $wire = '';
    while (!feof($fp)) {
        $chunk = @fread($fp, 8192);
        if ($chunk === false || $chunk === '') {
            break;
        }
        $wire .= $chunk;
    }
    fclose($fp);
    return $wire;
}

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $key = 'dGhlIHNhbXBsZSBub25jZQ==';
    $expectAccept = base64_encode(sha1($key . '258EAFA5-E914-47DA-95CA-C5AB0DC85B11', true));
    foreach ([
        ['name' => 'local', 'path' => '/throw-after-upgrade', 'exception' => true],
        ['name' => 'retained', 'path' => '/throw-after-upgrade-retained', 'exception' => true],
        ['name' => 'status-zero-retained', 'path' => '/return-after-upgrade-retained', 'exception' => false],
    ] as $case) {
        $name = $case['name'];
        $path = $case['path'];
        $wire = readUpgradedResponse($port, $path, $key);
        check($wire !== '', "$name upgrade closed with zero bytes");
        check(str_starts_with($wire, "HTTP/1.1 101 Switching Protocols\r\n"),
            "$name queued 101 was not flushed: " . var_export($wire, true));
        check(str_contains($wire, "Sec-WebSocket-Accept: $expectAccept"),
            "$name flushed 101 has the wrong Sec-WebSocket-Accept header");
        check(str_ends_with($wire, "\r\n\r\n"),
            "$name 101 head is incomplete: " . var_export($wire, true));
        check(substr_count($wire, 'HTTP/1.1 ') === 1,
            "$name received a second HTTP status after the hijack: " . var_export($wire, true));
        echo "$name-upgrade-101-flushed: ok\n";

        $exceptionLog =
            '/WARNING: .*\\[pool worker\\] http-direct worker: fpmng_worker_upgrade\\(\\) request id=\\d+ '
            . 'method=GET uri=' . preg_quote($path, '/') . ' was followed by a userland exception; '
            . 'outcome=close_after_write queued_bytes=\\d+/';
        if ($case['exception']) {
            $tester->expectLogPattern($exceptionLog, true);
            echo "$name-failed-upgrade-request-logged: ok\n";
        } else {
            $tester->expectNoLogPattern($exceptionLog, true);
            $tester->expectLogPattern(
                '/WARNING: .*\\[pool worker\\] http-direct worker: the worker script returned without '
                . 'being asked to stop \(exit status 0\)/',
                true
            );
            echo "$name-return-logged-without-exception-label: ok\n";
        }
    }
} finally {
    $tester->terminate();
    $tester->expectLogTerminatingNotices();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
local-upgrade-101-flushed: ok
local-failed-upgrade-request-logged: ok
retained-upgrade-101-flushed: ok
retained-failed-upgrade-request-logged: ok
status-zero-retained-upgrade-101-flushed: ok
status-zero-retained-return-logged-without-exception-label: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
