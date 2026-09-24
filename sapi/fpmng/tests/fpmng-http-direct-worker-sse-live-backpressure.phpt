--TEST--
fpm-ng: shipped SSE example retries a live stream after worker.send_buffer_limit backpressure (issue #454)
--SKIPIF--
<?php
include "skipif.inc";
if (!getenv('FPMNG_TEST_REPO_ROOT')) die('skip run through build/run-fpmng-phpt.sh');
?>
--ENV--
TEST_TIMEOUT=90
--FILE--
<?php
require_once "tester.inc";

function checkSse(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$repo = getenv('FPMNG_TEST_REPO_ROOT');
$work = sys_get_temp_dir() . '/fpmng-sse-live-backpressure-' . getmypid();
@mkdir($work, 0700, true);
$source = "$repo/examples/http-direct-worker-sse/app.php";
if (!is_file($source) || !copy($source, "$work/app.php")) {
    throw new RuntimeException("cannot copy shipped SSE example from $source");
}

$port = (int) (getenv('FPMNG_DIRECT_WORKER_SSE_BACKPRESSURE_PORT') ?: 28156);
$cfg = <<<CFG
[global]
error_log = {{FILE:LOG}}
pid = {{FILE:PID}}
[sse]
listen = 127.0.0.1:$port
pool.type = http-direct
pool.executor = worker
pm = static
pm.max_children = 1
chdir = $work
http.front_controller = /app.php
http.read_timeout = 30000
http.max_body = 1M
worker.max_pending = 8
worker.send_buffer_limit = 65536
php_admin_value[max_execution_time] = 0
CFG;

$tester = new FPM\Tester($cfg, '<?php echo "unused";');
$fp = null;
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    checkSse(is_resource($fp), "SSE connect failed: $errstr");
    stream_set_timeout($fp, 8);
    fwrite($fp, "GET /events?last_event_id=0 HTTP/1.1\r\nHost: test\r\nConnection: keep-alive\r\n\r\n");
    $status = fgets($fp);
    checkSse(is_string($status) && str_starts_with($status, 'HTTP/1.1 200'), 'SSE status: ' . var_export($status, true));
    while (($header = fgets($fp)) !== false && $header !== "\r\n") { /* leave the body unread */ }

    /* A 400 * 8 KiB batch exceeds the 64 KiB worker output cap. The stream
     * Fiber queues it synchronously during one heartbeat, before libevent gets
     * another loop turn to drain the output buffer, so at least one chunk hits
     * the backpressure return. Publish before the first 15s heartbeat,
     * so its heartbeat Fiber must hit backpressure while this client stays open. */
    $payload = str_repeat('x', 8192);
    for ($i = 1; $i <= 400; $i++) {
        $url = "http://127.0.0.1:$port/publish?msg=" . $payload . $i;
        $result = @file_get_contents($url);
        checkSse(is_string($result) && str_contains($result, "published $i"), "publish $i failed");
    }

    /* Let the 15-second heartbeat attempt the backlog while A is still open but
     * not reading. Then resume it and collect the queued backlog and heartbeat. */
    usleep(16_000_000);
    stream_set_blocking($fp, false);
    $received = '';
    $deadline = microtime(true) + 12;
    while (!str_contains($received, ": ping\n\n") && microtime(true) < $deadline) {
        $read = [$fp];
        $write = null;
        $except = null;
        $ready = @stream_select($read, $write, $except, 1, 0);
        if ($ready === false) break;
        if ($ready > 0) {
            $chunk = fread($fp, 131072);
            if ($chunk === false || $chunk === '') break;
            $received .= $chunk;
        }
    }

    checkSse(str_contains($received, "id: 400\ndata: " . $payload . "400\n\n"),
        'the resumed live reader did not receive the last queued SSE event');
    checkSse(str_contains($received, ": ping\n\n"),
        'the SSE Fiber did not continue to its heartbeat after draining the backlog');
    echo "live reader resumed through backpressure and received backlog + heartbeat: ok\n";

    fclose($fp);
    $fp = null;
    $published = @file_get_contents("http://127.0.0.1:$port/publish?msg=after-close");
    checkSse(is_string($published) && str_contains($published, 'published 401'),
        'worker stopped serving requests after the SSE client closed: ' . var_export($published, true));
    echo "closed stream does not prevent later request: ok\n";
    echo "Done\n";
} finally {
    if (is_resource($fp)) fclose($fp);
    $tester->terminate();
    $tester->close();
    @unlink("$work/app.php");
    @rmdir($work);
}
?>
--EXPECT--
live reader resumed through backpressure and received backlog + heartbeat: ok
closed stream does not prevent later request: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-sse-live-backpressure-*') as $dir) {
    if (@filemtime($dir) > $stale) continue;
    @unlink("$dir/app.php");
    @rmdir($dir);
}
?>
