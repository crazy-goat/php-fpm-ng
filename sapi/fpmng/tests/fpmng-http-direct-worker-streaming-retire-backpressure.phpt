--TEST--
fpm-ng: retiring a live stream after send-buffer backpressure ends it with a terminating chunk instead of waiting for SIGKILL (issue #459)
--SKIPIF--
<?php include "skipif.inc"; ?>
--ENV--
TEST_TIMEOUT=20
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$root = sys_get_temp_dir() . '/fpmng-worker-retire-backpressure-' . getmypid();
@mkdir($root, 0700, true);
$marker = "$root/backpressured.marker";

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    if (($env['REQUEST_URI'] ?? '/') === '/stall') {
        if (!fpmng_worker_respond_start($id, 200, [
            'Content-Type' => 'application/octet-stream',
        ])) {
            return;
        }
        $refused = !fpmng_worker_respond_chunk($id, str_repeat('X', 65536));
        file_put_contents(
            __DIR__ . '/backpressured.marker',
            getmypid() . ':' . ($refused ? 'refused' : 'accepted') . "\n",
            FILE_APPEND
        );
        /* Deliberately do not call respond_end(). A backpressured stream stays
         * retryable while the worker is healthy; after reload, may_exit() must
         * hand it to the SAPI's terminating-chunk teardown. */
        return;
    }
    fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], (string) getmypid());
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        handle($id);
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_STREAMING_RETIRE_BACKPRESSURE_PORT') ?: 28160);
$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
process_control_timeout = 2s
[worker]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
worker.send_buffer_limit = 16384
catch_workers_output = yes
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();
    @unlink($marker);

    /* Read the response head, then stop reading so the 64 KiB chunk cannot be
     * admitted under the 16 KiB send-buffer limit. */
    $stall = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    check((bool) $stall, "connect: $error");
    stream_set_timeout($stall, 5);
    fwrite($stall, "GET /stall HTTP/1.1\r\nHost: t\r\n\r\n");
    $status = fgets($stall);
    check(str_starts_with((string) $status, 'HTTP/1.1 200'), "status: $status");
    while (($line = fgets($stall)) !== false && $line !== "\r\n") { /* headers only */ }

    $deadline = microtime(true) + 5;
    while ((!is_file($marker) || trim((string) file_get_contents($marker)) === '')
        && microtime(true) < $deadline) {
        usleep(10000);
    }
    check(is_file($marker), 'the streaming handler never reported its chunk result');
    $result = trim((string) file_get_contents($marker));
    check(str_ends_with($result, ':refused'), "chunk was not backpressured: $result");
    $oldPid = (int) explode(':', $result, 2)[0];
    check($oldPid > 0, "worker pid missing from marker: $result");
    echo "backpressure-latched: ok\n";

    $started = microtime(true);
    $tester->reload();

    /* Resume immediately so the bounded shutdown flush can put the terminator
     * on the wire. A client that never reads cannot be promised delivery. */
    $tail = '';
    $deadline = $started + 5;
    while (microtime(true) < $deadline && !str_contains($tail, "0\r\n\r\n")) {
        $chunk = @fread($stall, 8192);
        if ($chunk === false || ($chunk === '' && feof($stall))) {
            break;
        }
        $tail .= $chunk;
    }
    check(str_contains($tail, "0\r\n\r\n"),
        'retirement did not deliver the terminating chunk: ' . var_export($tail, true));
    check(substr_count($tail, "0\r\n\r\n") === 1,
        'retirement delivered duplicate terminating chunks: ' . var_export($tail, true));
    fclose($stall);
    echo "terminating-chunk-after-retire: ok\n";

    $newBody = '';
    $deadline = $started + 5;
    while (microtime(true) < $deadline) {
        $newBody = @file_get_contents("http://127.0.0.1:$port/");
        if (is_string($newBody) && preg_match('/^\d+$/D', $newBody)) {
            break;
        }
        usleep(20000);
    }
    $elapsed = microtime(true) - $started;
    check(is_string($newBody) && preg_match('/^\d+$/D', $newBody),
        'replacement worker did not answer: ' . var_export($newBody, true));
    check((int) $newBody !== $oldPid, "worker pid did not change: $newBody");
    check($elapsed < 2.0, "retirement waited for the 2s master timeout: {$elapsed}s");
    echo "replacement-before-master-timeout: ok\n";

    $tester->expectNoLogPattern("/\\[pool worker\\] child $oldPid exited on signal 9/", true);
    $tester->expectLogPattern('/1 streamed response\(s\) still in progress; those were ended with their terminating chunk/', true);
    $tester->expectNoLogPattern('/response\(s\) were still unwritten after 1 s/', true);
    echo "clean-exit-no-sigkill: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @unlink($marker);
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
backpressure-latched: ok
terminating-chunk-after-retire: ok
replacement-before-master-timeout: ok
clean-exit-no-sigkill: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
