--TEST--
fpm-ng: an upgraded WebSocket closed while its unread input is at the bound releases its descriptor promptly (issue #974)
--SKIPIF--
<?php
include "skipif.inc";
if (!is_dir('/proc/self/fd')) {
    die("skip needs /proc to count the worker's descriptors");
}
?>
--FILE--
<?php

require_once "tester.inc";

function check(bool $condition, string $message): void
{
    if (!$condition) throw new RuntimeException($message);
}

/* Issue #974 bounds the unread input of an upgraded connection with a read
 * watermark. A stream closed while that input is at the bound never reads it
 * again, so the release linger (#472) has to see the peer's EOF through a read
 * that the watermark may still suspend. This test is a guard, not a regression
 * test for #974: the release path is the same with or without the bound, so it
 * passes on the control build too. It fails if the bound stops the descriptors
 * from being released, which would hold them for the linger's 5 s deadline. */

$root = sys_get_temp_dir() . '/fpmng-ws-input-close-' . getmypid();
@mkdir($root, 0700, true);

file_put_contents("$root/worker.php", <<<'PHP'
<?php
$notify = fpmng_worker_notify_stream();
$wss = [];

$watcher = fpmng_worker_event_create(FPMNG_WORKER_READ, $notify, function () use ($notify, &$wss): void {
    fread($notify, 65536);
    while (($id = fpmng_worker_next_request()) !== null) {
        $env = fpmng_worker_request_env($id);
        $uri = $env['REQUEST_URI'] ?? '/';

        if (str_contains($uri, '/ws')) {
            /* No watcher: nothing reads these streams, the input stays full. */
            $wss[] = fpmng_worker_upgrade($id, []);
            continue;
        }
        if (str_contains($uri, '/close-ws')) {
            $n = count($wss);
            foreach ($wss as $w) { fclose($w); }
            $wss = [];
            fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'closed:' . $n);
            continue;
        }
        fpmng_worker_respond($id, 200, ['Content-Type' => 'text/plain'], 'pid ' . getmypid());
    }
});
fpmng_worker_event_enable($watcher);

while (!fpmng_worker_may_exit()) {
    fpmng_worker_loop(true);
}
PHP);

$port = (int) (getenv('FPMNG_DIRECT_WORKER_WS_INPUT_CLOSE_PORT') ?: 28197 + 200 * (int) getenv('TEST_PHP_WORKER') + (int) getenv('FPMNG_PHPT_PORT_SHIFT'));
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
    while (($line = fgets($fp)) !== false && $line !== "\r\n") {
        /* skip headers */
    }
    return [$status];
}

function probe(int $port): string
{
    $fp = connect($port);
    fwrite($fp, "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    [$status] = readHead($fp);
    $body = stream_get_contents($fp);
    fclose($fp);
    check(str_starts_with($status, 'HTTP/1.1 200'), "probe status: $status");
    return trim((string) $body);
}

$tester = new FPM\Tester($config, '<?php echo "unused";');
try {
    $tester->start();
    $tester->expectLogStartNotices();

    $pid = (int) substr(probe($port), 4);
    $fds = fn() => count(scandir("/proc/$pid/fd")) - 2;
    $baseline = $fds();

    /* Three upgraded streams, each flooded until the socket refuses more: the
     * worker's buffer reaches the bound and the rest waits in the kernel. */
    $clients = [];
    for ($i = 0; $i < 3; $i++) {
        $ws = connect($port);
        fwrite($ws, "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            . "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
        readHead($ws);
        stream_set_blocking($ws, false);
        $sent = 0;
        while ($sent < 1024 * 1024) {
            $n = fwrite($ws, str_repeat('x', 65536));
            if (!$n) {
                break;
            }
            $sent += $n;
        }
        $clients[] = $ws;
    }
    usleep(300000);
    check($fds() >= $baseline + 3, 'the streams are not holding descriptors');

    /* The worker closes the streams without reading a byte. */
    $fp = connect($port);
    fwrite($fp, "GET /close-ws HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
    readHead($fp);
    $answer = trim((string) stream_get_contents($fp));
    fclose($fp);
    check($answer === 'closed:3', 'close-ws answered ' . var_export($answer, true));

    /* The peers go away. The worker must release the descriptors within 2 s;
     * a linger that cannot see EOF would hold them for its full 5 s deadline. */
    foreach ($clients as $c) {
        @stream_get_contents($c);
        fclose($c);
    }
    $deadline = microtime(true) + 2;
    while ($fds() > $baseline && microtime(true) < $deadline) {
        usleep(50000);
    }
    $after = $fds();
    check($after <= $baseline, "$after descriptors 2 s after the peers left, baseline $baseline");
    echo "closed-full-streams-release-fds: ok\n";

    check(probe($port) === "pid $pid", 'worker did not survive');
    echo "worker-survives: ok\n";
} finally {
    $tester->terminate();
    $tester->close();
    @unlink("$root/worker.php");
    @rmdir($root);
}
echo "Done\n";
?>
--EXPECT--
closed-full-streams-release-fds: ok
worker-survives: ok
Done
--CLEAN--
<?php require_once "tester.inc"; FPM\Tester::clean(); ?>
