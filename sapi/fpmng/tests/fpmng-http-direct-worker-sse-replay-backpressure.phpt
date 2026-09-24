--TEST--
fpm-ng: shipped SSE example replays a large Last-Event-ID backlog without abandoning a live reader (issue #455)
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

function checkSseReplay(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

$repo = getenv('FPMNG_TEST_REPO_ROOT');
$work = sys_get_temp_dir() . '/fpmng-sse-replay-backpressure-' . getmypid();
@mkdir($work, 0700, true);
$source = "$repo/examples/http-direct-worker-sse/app.php";
if (!is_file($source) || !copy($source, "$work/app.php")) {
    throw new RuntimeException("cannot copy shipped SSE example from $source");
}

$port = (int) (getenv('FPMNG_DIRECT_WORKER_SSE_REPLAY_PORT') ?: 28157);
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

    /* Preload 400 8 KiB events before reconnect. The replay Fiber's synchronous
     * burst exceeds the 64 KiB output cap before the event loop gets another
     * turn, even though the subscriber below reads continuously. */
    $payload = str_repeat('x', 8192);
    for ($i = 1; $i <= 400; $i++) {
        $url = "http://127.0.0.1:$port/publish?msg=" . $payload . $i;
        $result = @file_get_contents($url);
        checkSseReplay(is_string($result) && str_contains($result, "published $i"), "publish $i failed");
    }

    $fp = stream_socket_client("tcp://127.0.0.1:$port", $errno, $errstr, 5);
    checkSseReplay(is_resource($fp), "SSE reconnect failed: $errstr");
    fwrite($fp, "GET /events?last_event_id=0 HTTP/1.1\r\nHost: test\r\nConnection: keep-alive\r\n\r\n");
    $status = fgets($fp);
    checkSseReplay(is_string($status) && str_starts_with($status, 'HTTP/1.1 200'),
        'SSE status: ' . var_export($status, true));
    while (($header = fgets($fp)) !== false && $header !== "\r\n") { /* headers only */ }

    stream_set_blocking($fp, false);
    $received = '';
    $deadline = microtime(true) + 25;
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

    preg_match_all('/(?:^|\r?\n)id: (\d+)\r?\n/', $received, $matches);
    $ids = array_map('intval', $matches[1]);
    checkSseReplay(count($ids) === 400, 'expected one replay of 400 events, got ' . count($ids));
    checkSseReplay(count(array_unique($ids)) === 400, 'replay duplicated event ids');
    checkSseReplay(min($ids) === 1 && max($ids) === 400, 'replay missed an event id');
    checkSseReplay(str_contains($received, ": ping\n\n"), 'no heartbeat after replay');
    echo "continuous reader received all replay events once plus heartbeat: ok\n";

    fclose($fp);
    $fp = null;
    $published = @file_get_contents("http://127.0.0.1:$port/publish?msg=after-close");
    checkSseReplay(is_string($published) && str_contains($published, 'published 401'),
        'worker stopped serving after replay client closed: ' . var_export($published, true));
    echo "worker remains available after stream close: ok\n";
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
continuous reader received all replay events once plus heartbeat: ok
worker remains available after stream close: ok
Done
--CLEAN--
<?php
require_once "tester.inc";
FPM\Tester::clean();
$stale = time() - 300;
foreach (glob(sys_get_temp_dir() . '/fpmng-sse-replay-backpressure-*') as $dir) {
    if (@filemtime($dir) > $stale) continue;
    @unlink("$dir/app.php");
    @rmdir($dir);
}
?>
