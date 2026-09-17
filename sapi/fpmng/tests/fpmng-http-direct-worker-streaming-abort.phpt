--TEST--
fpm-ng: fpmng_worker_respond_chunk() returns false once the connection is gone (issue #332)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$root = sys_get_temp_dir() . '/fpmng-worker-streaming-abort-' . getmypid();
@mkdir($root, 0700, true);

/* /stream starts a response, sends one chunk, then waits on a libevent timer
 * (so the loop stays free to notice the client went away) before trying a
 * second chunk and an end(). The client closes the connection during that
 * wait, same as any reader that just stops reading mid-stream:
 * fpm_worker_conn_closed() (armed at accept time, still armed while
 * p->streaming) clears p->http, and both calls after that must report false
 * -- not crash, not write to a dead connection -- exactly like
 * fpmng_worker_respond() already does outside a stream. */
file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function waitThenContinue(int $id): void
{
    $box = new stdClass();
    $box->id = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use ($id, $box) {
        fpmng_worker_event_free($box->id);
        $ok2 = fpmng_worker_respond_chunk($id, 'second');
        $okEnd = fpmng_worker_respond_end($id);
        fwrite(STDERR, "post-close: chunk=" . var_export($ok2, true) . " end=" . var_export($okEnd, true) . "\n");
    });
    fpmng_worker_event_enable($box->id, 0.5);
}

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    if (($env['REQUEST_URI'] ?? '/') === '/stream') {
        fpmng_worker_respond_start($id, 200, ['Content-Type' => 'text/plain']);
        fpmng_worker_respond_chunk($id, 'first');
        waitThenContinue($id);
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

$port = (int) (getenv('FPMNG_DIRECT_WORKER_STREAMING_ABORT_PORT') ?: 28095);
$config = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[streamed]
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
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    check((bool) $fp, "connect: $error");
    fwrite($fp, "GET /stream HTTP/1.1\r\nHost: t\r\n\r\n");
    /* Read the status line, the one chunk we know is coming, then close
     * without reading the rest -- exactly the "client is gone mid-stream"
     * case fpmng_worker_respond_chunk() must survive. */
    $line = fgets($fp);
    check(str_starts_with((string) $line, 'HTTP/1.1 200'), "status: $line");
    while (($l = fgets($fp)) !== false && $l !== "\r\n") { /* drain headers */ }
    fgets($fp); /* chunk-size line for "first" */
    fread($fp, 5); /* "first" */
    fgets($fp); /* trailing CRLF */
    fclose($fp);

    /* Give the worker's timer and the two post-close calls time to run. */
    usleep(1200 * 1000);

    /* catch_workers_output routes the worker's STDERR into the error log, so
     * the post-close call results the timer callback printed land there. */
    $tester->expectLogPattern('/post-close: chunk=false end=false/', true);
    echo "post-close-chunk-returns-false: ok\n";
    echo "post-close-end-returns-false: ok\n";

    /* The worker is still alive and answers a fresh request normally: the
     * aborted stream's pending entry did not leak or wedge anything. */
    $again = file_get_contents("http://127.0.0.1:$port/");
    check(is_string($again) && str_starts_with($again, 'hello from pid '), 'again: ' . var_export($again, true));
    echo "worker-still-serving-after-abort: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
post-close-chunk-returns-false: ok
post-close-end-returns-false: ok
worker-still-serving-after-abort: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
