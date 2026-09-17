--TEST--
fpm-ng: worker.send_buffer_limit bounds fpmng_worker_respond_chunk() backpressure (issue #332)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$root = sys_get_temp_dir() . '/fpmng-worker-streaming-backpressure-' . getmypid();
@mkdir($root, 0700, true);

/* /stall starts a response, then hammers fpmng_worker_respond_chunk() with
 * 64 KiB pieces against a client that never reads. worker.send_buffer_limit
 * is a small multiple of one piece, so the queued-but-unwritten bufferevent
 * output crosses it well before the handler runs out of pieces to offer --
 * the refusal has to be reported by the return value, not thrown, and it must
 * stop the handler from queuing indefinitely into memory. */
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    if (($env['REQUEST_URI'] ?? '/') === '/stall') {
        fpmng_worker_respond_start($id, 200, ['Content-Type' => 'application/octet-stream']);
        $piece = str_repeat('X', 65536);
        $sent = 0;
        $refusedAt = null;
        for ($i = 0; $i < 256; $i++) {
            if (!fpmng_worker_respond_chunk($id, $piece)) {
                $refusedAt = $i;
                break;
            }
            $sent++;
        }
        fwrite(STDERR, "stall-result: sent=$sent refused=" . var_export($refusedAt, true) . "\n");
        fpmng_worker_respond_end($id);
        return;
    }
    fpmng_worker_respond($id, 200, [], 'hello from pid ' . getmypid());
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

$port = (int) (getenv('FPMNG_DIRECT_WORKER_STREAMING_BACKPRESSURE_PORT') ?: 28096);
$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[stalled]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $root
http.front_controller = /worker.php
http.read_timeout = 10000
http.max_body = 1M
catch_workers_output = yes
worker.send_buffer_limit = 131072
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    /* A connection that reads the status line and headers, then stops reading
     * entirely -- the non-reading client worker.send_buffer_limit exists for. */
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    check((bool) $fp, "connect: $error");
    fwrite($fp, "GET /stall HTTP/1.1\r\nHost: t\r\n\r\n");
    $line = fgets($fp);
    check(str_starts_with((string) $line, 'HTTP/1.1 200'), "status: $line");
    while (($l = fgets($fp)) !== false && $l !== "\r\n") { /* drain headers, never read the body */ }

    /* Give the worker's synchronous burst of chunk() calls time to run and be
     * logged, without this test process ever reading the streamed body. */
    usleep(500 * 1000);

    $tester->expectLogPattern('/stall-result: sent=(\d+) refused=(\d+)/', true);
    echo "backpressure-refused-a-chunk: ok\n";

    fclose($fp);

    /* The worker survives having refused a chunk mid-stream: a fresh request
     * on a fresh connection still gets a normal answer. */
    $again = file_get_contents("http://127.0.0.1:$port/");
    check(is_string($again) && str_starts_with($again, 'hello from pid '), 'again: ' . var_export($again, true));
    echo "worker-still-serving-after-backpressure: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
backpressure-refused-a-chunk: ok
worker-still-serving-after-backpressure: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
