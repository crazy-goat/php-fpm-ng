--TEST--
fpm-ng: SSE streams on pool.executor = worker — held while ordinary requests are answered, exempt from worker.request_timeout, and ended with a clean terminating chunk when the worker stops (issue #342)
--SKIPIF--
<?php include "skipif.inc"; ?>
--FILE--
<?php
require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* The three #342 semantics in one run:
 * 1. Two held SSE streams do not stop ordinary requests from being answered
 *    by the same worker, and a stream outlives worker.request_timeout = 1
 *    without a 504 (a started stream is exempt from that deadline).
 * 2. The worker script exits while both streams are still open -- the SAPI's
 *    fpm_worker_finish_output() must end each of them with its terminating
 *    chunk ("0\r\n\r\n"), not the pre-#342 shutdown() reset, so an
 *    EventSource reconnects instead of reporting a broken connection. */
$root = sys_get_temp_dir() . '/fpmng-worker-sse-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();

function waitFor(float $seconds): void
{
    $fiber = Fiber::getCurrent();
    $box = new stdClass();
    $box->id = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use ($fiber, $box) {
        fpmng_worker_event_free($box->id);
        $fiber->resume();
    });
    fpmng_worker_event_enable($box->id, $seconds);
    Fiber::suspend();
}

function handle(int $id): void
{
    $env = fpmng_worker_request_env($id);
    $uri = $env['REQUEST_URI'] ?? '/';

    if (str_starts_with($uri, '/events')) {
        if (!fpmng_worker_respond_start($id, 200, [
            'Content-Type' => 'text/event-stream',
            'Cache-Control' => 'no-cache',
        ])) {
            return;
        }
        /* Tell the harness this stream is genuinely open, then send one event
         * so the harness can read a first frame before it proceeds. */
        file_put_contents(__DIR__ . '/streams.marker', $id . "\n", FILE_APPEND);
        if (!fpmng_worker_respond_chunk($id, "id: 0\nevent: hello\ndata: start\n\n")) {
            return;
        }
        /* Hold the stream well past worker.request_timeout (= 1 s below):
         * heartbeat comments every 200 ms, p->streaming keeps the 504 sweep
         * away. Never ends on its own -- the script below exits the worker
         * with the stream still open. */
        while (true) {
            waitFor(0.2);
            if (!fpmng_worker_respond_chunk($id, ": ping\n\n")) {
                return;
            }
        }
    }
    fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'hello from pid ' . getmypid());
}

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        (new Fiber(function () use ($id) {
            try {
                handle($id);
            } catch (Throwable $e) {
                fpmng_worker_respond($id, 500, [], 'handler failed');
            }
        }))->start();
    }
});
fpmng_worker_event_enable($watcher);

while (true) {
    fpmng_worker_loop(true);
    /* The harness drops this marker once both streams have delivered their
     * first event and the ordinary requests have been answered. Exiting here
     * is the abrupt case on purpose: fpm_worker_finish_output() inherits two
     * streaming entries and must end them cleanly (issue #342). */
    if (file_exists(__DIR__ . '/stop.marker') && count(file(__DIR__ . '/streams.marker')) >= 2) {
        exit(0);
    }
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_SSE_PORT') ?: 28142);
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
http.max_body = 1M
worker.request_timeout = 1
catch_workers_output = yes
php_admin_value[max_execution_time] = 0
php_admin_value[display_errors] = 0
CFG;

function connect(int $port)
{
    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    if (!$fp) throw new RuntimeException("connect :$port: $error");
    stream_set_timeout($fp, 10);
    return $fp;
}

/* Sends the request and returns the connection still open: the stream is held,
 * nothing is read yet. */
function startStream(int $port, string $path)
{
    $fp = connect($port);
    fwrite($fp, "GET $path HTTP/1.1\r\nHost: t\r\n\r\n");
    return $fp;
}

/* Waits until the connection has produced $needle, buffering the rest. */
function readUntil($fp, string $needle, string $what): string
{
    $buffer = '';
    $deadline = microtime(true) + 10;
    while (microtime(true) < $deadline && !str_contains($buffer, $needle)) {
        $chunk = fread($fp, 8192);
        if ($chunk === false || ($chunk === '' && feof($fp))) {
            break;
        }
        $buffer .= $chunk;
        if (!str_contains($buffer, $needle)) {
            usleep(20000);
        }
    }
    check(str_contains($buffer, $needle), "stream never delivered $what: " . var_export($buffer, true));
    return $buffer;
}

$tester = new FPM\Tester($config, '<?php');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    @unlink("$root/streams.marker");
    @unlink("$root/stop.marker");

    /* Two held streams. */
    $sse1 = startStream($port, '/events');
    $sse2 = startStream($port, '/events');
    $deadline = microtime(true) + 10;
    while ((!file_exists("$root/streams.marker") || count(file("$root/streams.marker")) < 2)
        && microtime(true) < $deadline) {
        usleep(20000);
    }
    check(file_exists("$root/streams.marker") && count(file("$root/streams.marker")) >= 2,
        'the two /events handlers never reported themselves streaming');

    /* First event on each stream, while the second is held. */
    readUntil($sse1, 'event: hello', 'its first event');
    readUntil($sse2, 'event: hello', 'its first event');
    echo "streams-open-with-first-event: ok\n";

    /* Ordinary requests are answered while both streams are held -- the whole
     * point of the worker executor -- and they keep being answered past
     * worker.request_timeout = 1 s, proving the streams were not what the
     * sweep killed (and that no 504 touched them). */
    $deadline = microtime(true) + 5;
    while (microtime(true) < $deadline) {
        usleep(100000);    /* > worker.request_timeout */
        $body = @file_get_contents("http://127.0.0.1:$port/");
        if (is_string($body) && str_starts_with($body, 'hello from pid ')) {
            break;
        }
    }
    $body = @file_get_contents("http://127.0.0.1:$port/");
    check(is_string($body) && str_starts_with($body, 'hello from pid '),
        'an ordinary request was not answered while two streams were held: ' . var_export($body, true));
    echo "ordinary-requests-answered-past-request-timeout: ok\n";

    /* Now let the worker script exit with both streams still open. */
    touch("$root/stop.marker");

    /* Each stream must end with a COMPLETE chunked message: the terminating
     * chunk "0\r\n\r\n", not a reset. A client that saw the old pre-#342
     * behaviour would get fread() === false/feof with no terminator. */
    foreach ([1 => $sse1, 2 => $sse2] as $i => $fp) {
        readUntil($fp, "0\r\n\r\n", "its terminating chunk (stream $i)");
    }
    echo "streams-ended-with-terminating-chunk: ok\n";
    fclose($sse1);
    fclose($sse2);

    /* The SAPI's own record: #342 replaced the "cut short" warning with a
     * notice that says what actually happened. */
    $tester->expectNoLogPattern('/cut short/');
    $tester->expectLogPattern('/ended with their terminating chunk/');
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @unlink("$root/streams.marker");
    @unlink("$root/stop.marker");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
streams-open-with-first-event: ok
ordinary-requests-answered-past-request-timeout: ok
streams-ended-with-terminating-chunk: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
