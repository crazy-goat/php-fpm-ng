--TEST--
fpm-ng: a dead SSE client's pending entry is reaped server-side, so a driver that drops the id cannot burn its slots into 503 (issue #444)
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

/* Issue #444: fpm_worker_conn_closed() recorded the dead id in the closed ring
 * but never reaped the entry. The #342 contract invites the driver to DROP the
 * subscription on fpmng_worker_closed_requests() and never respond for that id
 * -- and the zombie {streaming = true, http = NULL} then held its
 * worker.max_pending slot for the worker's whole life: the churn of dead
 * clients saturated the accept ceiling into 503 "Worker unavailable" and a
 * forced recycle (confirmed, zero real load). The callback reaps the entry now;
 * this test drives exactly that driver shape. */

$root = sys_get_temp_dir() . '/fpmng-sse-dead-client-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$subs = [];

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, &$subs): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        $uri = $env['REQUEST_URI'] ?? '/';

        if (str_starts_with($uri, '/events')) {
            if (!fpmng_worker_respond_start($id, 200, [
                'Content-Type' => 'text/event-stream',
                'Cache-Control' => 'no-cache',
            ])) {
                continue;
            }
            $subs[$id] = true;
            file_put_contents(__DIR__ . '/streams.marker', "$id\n", FILE_APPEND);
            if (!fpmng_worker_respond_chunk($id, "id: 0\nevent: hello\ndata: start\n\n")) {
                return;
            }
            continue;
        }
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'pid ' . getmypid());
    }
});
fpmng_worker_event_enable($watcher);

/* The #342 driver shape: on fpmng_worker_closed_requests() the subscription is
 * DROPPED -- the id is never responded for again (issue #444). */
$drain = fpmng_worker_event_create(FPMNG_WORKER_TIMER, null, function () use (&$subs, &$drain): void {
    foreach (fpmng_worker_closed_requests() as $cid) {
        unset($subs[$cid]);
        file_put_contents(__DIR__ . '/closed.marker', "$cid\n", FILE_APPEND);
    }
    fpmng_worker_event_enable($drain, 0.05);
});
fpmng_worker_event_enable($drain, 0.05);

while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_SSE_DEAD_SLOT_PORT') ?: 28148);
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
worker.max_pending = 3
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

function readHead($fp): array
{
    $line = fgets($fp);
    if (!$line || !str_starts_with($line, 'HTTP/')) {
        throw new RuntimeException('bad status line: ' . var_export($line, true));
    }
    $status = trim($line);
    $headers = [];
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        [$k, $v] = explode(':', $line, 2);
        $headers[strtolower(trim($k))] = trim($v);
    }
    return [$status, $headers];
}

function startStream(int $port)
{
    $fp = connect($port);
    fwrite($fp, "GET /events HTTP/1.1\r\nHost: t\r\nAccept: text/event-stream\r\n\r\n");
    [$status, ] = readHead($fp);
    check(str_starts_with($status, 'HTTP/1.1 200'), "stream status: $status");
    /* The first event frame: proof the stream is genuinely open and holding a
     * slot. */
    $frame = '';
    while (!str_contains($frame, "\n\n")) {
        $part = fread($fp, 256);
        if ($part === false || $part === '') throw new RuntimeException('no first event');
        $frame .= $part;
    }
    check(str_contains($frame, 'event: hello'), 'first event: ' . var_export($frame, true));
    return $fp;
}

function probe(int $port): array
{
    $fp = connect($port);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    [$status, ] = readHead($fp);
    $body = stream_get_contents($fp);
    fclose($fp);
    return [$status, trim((string) $body)];
}

function waitFor(string $path, int $lines): void
{
    for ($i = 0; $i < 100; $i++) {
        $c = @file($path, FILE_IGNORE_NEW_LINES);
        if ($c !== false && count($c) >= $lines) {
            return;
        }
        usleep(50000);
    }
    throw new RuntimeException("timeout waiting for $lines line(s) in $path");
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    [, $pid0] = probe($port);
    check(str_starts_with($pid0, 'pid '), 'probe body: ' . var_export($pid0, true));

    /* Three healthy subscribers hold all three worker.max_pending slots. */
    $streams = [startStream($port), startStream($port), startStream($port)];
    waitFor("$root/streams.marker", 3);
    echo "three-subscribers: ok\n";

    /* Every client walks away mid-stream. */
    foreach ($streams as $fp) {
        fclose($fp);
    }
    waitFor("$root/closed.marker", 3);
    /* Let the close callbacks and the reap settle. */
    usleep(300000);

    /* The driver dropped the ids -- the slots must be free again: the ordinary
     * request answers 200 on the SAME pid (pre-fix: 503 Worker unavailable),
     * and a new subscriber gets a slot. */
    [$status, $body] = probe($port);
    check(str_starts_with($status, 'HTTP/1.1 200'), "post-churn ordinary request: $status $body");
    check($body === $pid0, "post-churn pid changed: $pid0 vs $body");
    echo "slots-reaped-after-churn: ok\n";

    $fp = startStream($port);
    echo "new-subscriber: ok\n";
    fclose($fp);
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    foreach (['streams.marker', 'closed.marker'] as $m) {
        @unlink("$root/$m");
    }
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
three-subscribers: ok
slots-reaped-after-churn: ok
new-subscriber: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
